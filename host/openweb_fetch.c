/*
 * openweb.fetch/1 on the host (openweb_fetch.h, docs/OPENWEB_FETCH.md).
 *
 * A fetch is a struct fetch in a table of 64 slots, found by its handle
 * (the slot and a generation, so a stale handle never reaches a new fetch).
 * OPEN checks the request and starts the fetch's thread, which resolves the
 * name, connects, does the TLS handshake, sends the request and its body,
 * reads the head and then the body into a 1 MB window. The calls on the
 * card's workers only wait on the fetch's condition variable, for at most
 * the wait the Amiga gave, and copy out of the window.
 *
 * Lifetime: a fetch has a reference for its slot, one for its thread and
 * one for each call in progress, all counted under the table's mutex. The
 * socket is closed only by the thread, under the fetch's mutex, so CANCEL
 * (shutdown, under the same mutex) never reaches a reused descriptor.
 *
 * Held calls (wf_start): a HEAD or READ with nothing to give yet is kept on
 * its fetch, under the fetch's mutex, and answered by whichever thread
 * changes what it waits for (the fetch's thread, a CANCEL or CLOSE), or by
 * the timer thread when its wait runs out; wf_cancel answers it at once.
 * CANCEL and CLOSE answer every held call of their fetch, so none outlives
 * its slot. The card's completion is called under the fetch's mutex; it
 * takes only the card's own lock and never calls back in.
 *
 * Kept connections: after an answer framed by its length or chunks, read to
 * its end, with neither side asking to close, the connection goes to a
 * pool (per scheme, host and port: at most KEEP_PER_HOST, KEEP_MAX in all)
 * and the next fetch to the same place without a body takes it. A kept
 * connection is checked before it is used; one the server closed in the
 * meantime is dropped, and if it fails before a byte of the answer comes,
 * the request goes again on a new connection. The timer thread closes those
 * idle for KEEP_IDLE seconds, or the server's Keep-Alive timeout if less.
 * A fetch's descriptor is set to -1 under its mutex before its connection
 * is kept, so a late CANCEL never reaches the next fetch on it.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#define _GNU_SOURCE
#include "openweb_fetch.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <zlib.h>
#ifdef WF_BROTLI
#include <brotli/decode.h>
#define ACCEPT_ENCODING "gzip, deflate, br"
#else
#define ACCEPT_ENCODING "gzip, deflate"
#endif

#define MAX_FETCHES  64
#define MAX_TEXT     (64u << 10)   /* the request text; a response head */
#define WINDOW       (1u << 20)    /* body bytes read ahead of the Amiga */
#define UPLOAD_MAX   (1u << 20)    /* body bytes queued to send */
#define MAX_WAIT_MS  2000u
#define TIMEOUT_DEF  30u
#define TIMEOUT_MAX  300u
#define IDLE_CLOSE   600           /* seconds untouched before the host closes a fetch itself */
#define IOBUF        16384u
#define INFO_ROOM    1536u
#define KEEP_PER_HOST 6            /* idle connections kept for one scheme, host and port */
#define KEEP_MAX     32            /* idle connections kept in all */
#define KEEP_IDLE    30            /* seconds one is kept idle, unless the server's Keep-Alive says less */

struct piece {
    struct piece *next;
    uint32_t length;
    uint8_t data[];
};

/* A HEAD or READ held for wf_start (under its fetch's mutex). */
struct held {
    struct held *next;
    uint32_t id;                /* the card's */
    uint16_t op;
    uint8_t *out;               /* buf[0].out: the card's copy, ours until done */
    uint32_t room;
    struct timespec until;      /* the wait runs out */
    wf_done_fn done;
};

struct fetch {
    pthread_mutex_t mtx;
    pthread_cond_t cv;          /* any change: head, body bytes, room, upload, cancel, end */
    int refs;                   /* under table_mtx */

    /* The request: set by OPEN, then the thread's. */
    char method[17];
    int tls;
    char host[256], port[8];
    char *target, *request;     /* "/path?q", and the header lines to send (CRLF) */
    uint32_t flags, timeout;
    uint64_t body_length;       /* WF_LENGTH_UNKNOWN: chunked */

    /* Under mtx. */
    struct piece *up_head, *up_tail;
    uint32_t up_bytes;
    uint64_t up_taken;          /* body bytes the Amiga has given */
    int up_last;                /* the Amiga has given the last part */
    int up_closed;              /* the thread takes no more: later parts are dropped */
    int fd;                     /* -1: none */
    int cancelled, running;
    time_t touched;
    uint32_t error;             /* the fetch failed: WF_E_* */
    char info[INFO_ROOM];       /* key=value lines */
    int head_ready;             /* status, hflags, version, htext, length are set */
    uint32_t status, hflags, version;
    char *htext;
    uint32_t hlen;
    uint64_t length;            /* with WF_H_LENGTH */
    int decoded;                /* the body is being decoded */
    uint8_t *ring;              /* the window */
    uint32_t r_at, r_len;
    uint64_t body_total;        /* bytes put in the window */
    int body_done;              /* nothing more will come (error says whether it failed) */
    struct held *held;          /* calls waiting to be answered, oldest first */
};

static pthread_mutex_t table_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct { struct fetch *f; uint32_t gen; } slots[MAX_FETCHES];
static int threads_running;

static void serve_held_locked(struct fetch *f, const struct timespec *now);
static int timer_ready(void);
static void timer_poke_at(const struct timespec *t);

static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static SSL_CTX *tls_ctx;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int ts_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static struct timespec deadline_ms(uint32_t ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

static time_t now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static void fetch_free(struct fetch *f)
{
    while (f->held) {                           /* none should be left (CANCEL and CLOSE answer them); never leave one unanswered */
        struct held *h = f->held;
        f->held = h->next;
        h->done(h->id, WF_CALL_CANCELLED, 0, 0);
        free(h);
    }
    while (f->up_head) {
        struct piece *p = f->up_head;
        f->up_head = p->next;
        free(p);
    }
    free(f->target);
    free(f->request);
    free(f->htext);
    free(f->ring);
    pthread_cond_destroy(&f->cv);
    pthread_mutex_destroy(&f->mtx);
    free(f);
}

/* Drops one reference; the last frees it. */
static void release(struct fetch *f)
{
    int last;
    pthread_mutex_lock(&table_mtx);
    last = --f->refs == 0;
    pthread_mutex_unlock(&table_mtx);
    if (last)
        fetch_free(f);
}

/* The fetch behind a handle, with a reference for the caller; NULL if none. */
static struct fetch *lookup(uint32_t handle)
{
    uint32_t i = (handle & 0xff) - 1;
    struct fetch *f = NULL;
    pthread_mutex_lock(&table_mtx);
    if (i < MAX_FETCHES && slots[i].f && slots[i].gen == handle >> 8) {
        f = slots[i].f;
        f->refs++;
    }
    pthread_mutex_unlock(&table_mtx);
    if (f) {
        pthread_mutex_lock(&f->mtx);
        f->touched = now_s();
        pthread_mutex_unlock(&f->mtx);
    }
    return f;
}

/* Under f->mtx: something a HEAD or READ waits for changed. Wakes the calls
 * waiting on a worker and answers the held ones that can be answered now. */
static void changed_locked(struct fetch *f)
{
    pthread_cond_broadcast(&f->cv);
    if (f->held)
        serve_held_locked(f, NULL);
}

/* Under f->mtx: the fetch stops. A failure before the end keeps its first error. */
static void cancel_locked(struct fetch *f)
{
    if (f->cancelled)
        return;
    f->cancelled = 1;
    if (!f->body_done && !f->error) {
        size_t n = strlen(f->info);
        f->error = WF_E_CANCELLED;
        snprintf(f->info + n, sizeof f->info - n, "error=cancelled\n");
    }
    if (f->fd >= 0)
        shutdown(f->fd, SHUT_RDWR);
    changed_locked(f);                          /* every held call: a cancelled fetch has its answer */
}

/* Under f->mtx, from the thread: the fetch failed with err; why is one line. */
static void fail_locked(struct fetch *f, uint32_t err, const char *why)
{
    if (!f->error) {
        size_t n = strlen(f->info);
        f->error = err;
        snprintf(f->info + n, sizeof f->info - n, "error=%s\n", why);
    }
    f->body_done = 1;
    changed_locked(f);
}

static void fail(struct fetch *f, uint32_t err, const char *why)
{
    pthread_mutex_lock(&f->mtx);
    fail_locked(f, f->cancelled ? WF_E_CANCELLED : err, f->cancelled ? "cancelled" : why);
    pthread_mutex_unlock(&f->mtx);
}

static int is_cancelled(struct fetch *f)
{
    int c;
    pthread_mutex_lock(&f->mtx);
    c = f->cancelled;
    pthread_mutex_unlock(&f->mtx);
    return c;
}

/* ---- TLS ------------------------------------------------------------------------- */

/* Every certificate passes the handshake here, so the thread can say what
 * was wrong with one and summarise it; SSL_get_verify_result then decides,
 * before a byte of the request is sent. */
static int verify_later(int ok, X509_STORE_CTX *ctx)
{
    (void)ok;
    (void)ctx;
    return 1;
}

static void tls_init(void)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    static const unsigned char alpn[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };
    if (!ctx)
        return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_later);
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof alpn);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        SSL_CTX_free(ctx);
        return;
    }
    tls_ctx = ctx;
}

/* A name or a time from a certificate, as one line. */
static void name_line(char *out, size_t room, const char *key, X509_NAME *name)
{
    BIO *b = BIO_new(BIO_s_mem());
    char *p = NULL;
    long n;
    out[0] = 0;
    if (!b)
        return;
    if (name && X509_NAME_print_ex(b, name, 0, XN_FLAG_RFC2253) >= 0 && (n = BIO_get_mem_data(b, &p)) > 0)
        snprintf(out, room, "%s=%.*s\n", key, (int)(n > 300 ? 300 : n), p);
    BIO_free(b);
}

static void time_line(char *out, size_t room, const char *key, const ASN1_TIME *t)
{
    BIO *b = BIO_new(BIO_s_mem());
    char *p = NULL;
    long n;
    out[0] = 0;
    if (!b)
        return;
    if (t && ASN1_TIME_print(b, t) && (n = BIO_get_mem_data(b, &p)) > 0)
        snprintf(out, room, "%s=%.*s\n", key, (int)(n > 64 ? 64 : n), p);
    BIO_free(b);
}

/* Under f->mtx: the certificate's summary appended to the info text. */
static void summarise_locked(struct fetch *f, X509 *cert)
{
    char line[400];
    size_t n;
    if (!cert)
        return;
    name_line(line, sizeof line, "subject", X509_get_subject_name(cert));
    n = strlen(f->info);
    snprintf(f->info + n, sizeof f->info - n, "%s", line);
    name_line(line, sizeof line, "issuer", X509_get_issuer_name(cert));
    n = strlen(f->info);
    snprintf(f->info + n, sizeof f->info - n, "%s", line);
    time_line(line, sizeof line, "expires", X509_get0_notAfter(cert));
    n = strlen(f->info);
    snprintf(f->info + n, sizeof f->info - n, "%s", line);
}

/* ---- the connection ------------------------------------------------------------------- */

struct conn {
    int fd;
    SSL *ssl;
    int timedout;
    int reused;                 /* taken from the pool for this request (and not failed over yet) */
    int heard;                  /* a byte has come on it for this request */
    int spent;                  /* it ended unclean (a chunked body's trailers cut off): not kept */
    uint32_t pos, len;
    char tls[INFO_ROOM];        /* the info lines of its handshake, kept with it */
    uint8_t buf[IOBUF];
};

/* Raw bytes: > 0 read, 0 the end, -1 an error (c->timedout when it was the timeout). */
static int conn_read(struct conn *c, void *p, int n)
{
    for (;;) {
        if (c->ssl) {
            int r, e;
            ERR_clear_error();
            r = SSL_read(c->ssl, p, n);
            if (r > 0)
                return r;
            e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN)
                return 0;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE
                || (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)))
                c->timedout = 1;
            return -1;
        } else {
            ssize_t r = recv(c->fd, p, (size_t)n, 0);
            if (r >= 0)
                return (int)r;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                c->timedout = 1;
            return -1;
        }
    }
}

static int conn_write(struct conn *c, const void *p, size_t n)
{
    const uint8_t *d = p;
    while (n) {
        int k = n > (1u << 30) ? (1 << 30) : (int)n;
        if (c->ssl) {
            int r;
            ERR_clear_error();
            r = SSL_write(c->ssl, d, k);
            if (r <= 0) {
                int e = SSL_get_error(c->ssl, r);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE
                    || (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)))
                    c->timedout = 1;
                return -1;
            }
            k = r;
        } else {
            ssize_t r = send(c->fd, d, (size_t)k, MSG_NOSIGNAL);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    c->timedout = 1;
                return -1;
            }
            k = (int)r;
        }
        d += k;
        n -= (size_t)k;
    }
    return 0;
}

/* More bytes into the buffer: > 0 how many, 0 the end, -1 an error. */
static int conn_fill(struct conn *c)
{
    int r;
    if (c->pos && c->pos == c->len)
        c->pos = c->len = 0;
    if (c->pos && c->len == sizeof c->buf) {
        memmove(c->buf, c->buf + c->pos, c->len - c->pos);
        c->len -= c->pos;
        c->pos = 0;
    }
    if (c->len == sizeof c->buf)
        return -1;
    r = conn_read(c, c->buf + c->len, (int)(sizeof c->buf - c->len));
    if (r > 0) {
        c->len += (uint32_t)r;
        c->heard = 1;
    }
    return r;
}

/* One line (without its LF and CR) into out: 1, 0 the end, -1 an error, -2 too long. */
static int conn_line(struct conn *c, char *out, size_t room)
{
    size_t n = 0;
    for (;;) {
        while (c->pos < c->len) {
            uint8_t ch = c->buf[c->pos++];
            if (ch == '\n') {
                if (n && out[n - 1] == '\r')
                    n--;
                out[n] = 0;
                return 1;
            }
            if (n + 1 >= room)
                return -2;
            out[n++] = (char)ch;
        }
        {
            int r = conn_fill(c);
            if (r <= 0)
                return r;
        }
    }
}

/* Why a read or write failed, for the info text: the fetch's error and its words. */
static void conn_fail(struct fetch *f, struct conn *c, uint32_t otherwise, const char *what)
{
    char why[160];
    if (c->timedout) {
        snprintf(why, sizeof why, "timed out after %u s %s", f->timeout, what);
        fail(f, WF_E_TIMEOUT, why);
    } else {
        unsigned long e = c->ssl ? ERR_peek_last_error() : 0;
        if (e)
            snprintf(why, sizeof why, "%s %s", ERR_reason_error_string(e) ? ERR_reason_error_string(e) : "TLS error", what);
        else
            snprintf(why, sizeof why, "the connection closed %s", what);
        fail(f, otherwise, why);
    }
}

/* ---- connecting ---------------------------------------------------------------------- */

/* A connected socket to one of the name's addresses, in blocking mode with
 * the timeout on reads and writes; -1 when it failed (the fetch says why). */
static int dial(struct fetch *f)
{
    struct addrinfo hints, *list = NULL, *a;
    char why[400];
    int rc, fd = -1, last_err = 0, timed_out = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    rc = getaddrinfo(f->host, f->port, &hints, &list);
    if (rc) {
        snprintf(why, sizeof why, "%s: %s", f->host, rc == EAI_SYSTEM ? strerror(errno) : gai_strerror(rc));
        fail(f, WF_E_DNS, why);
        return -1;
    }
    for (a = list; a && fd < 0; a = a->ai_next) {
        struct timespec until = deadline_ms(f->timeout * 1000), t;
        int s = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
        if (s < 0) {
            last_err = errno;
            continue;
        }
        pthread_mutex_lock(&f->mtx);
        if (f->cancelled) {
            pthread_mutex_unlock(&f->mtx);
            close(s);
            break;
        }
        f->fd = s;                              /* so CANCEL can shut it */
        pthread_mutex_unlock(&f->mtx);
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0)
            last_err = 0;
        else if (errno != EINPROGRESS)
            last_err = errno;
        else {
            last_err = ETIMEDOUT;
            for (;;) {
                struct pollfd p = { s, POLLOUT, 0 };
                long left;
                clock_gettime(CLOCK_MONOTONIC, &t);
                left = (until.tv_sec - t.tv_sec) * 1000 + (until.tv_nsec - t.tv_nsec) / 1000000;
                if (left <= 0)
                    break;
                if (poll(&p, 1, left > 100 ? 100 : (int)left) > 0) {
                    socklen_t len = sizeof last_err;
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &last_err, &len))
                        last_err = errno;
                    break;
                }
                if (is_cancelled(f))
                    break;
            }
            if (last_err == ETIMEDOUT)
                timed_out = 1;
        }
        if (!last_err && !is_cancelled(f)) {
            struct timeval tv = { (time_t)f->timeout, 0 };
            int one = 1;
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            fd = s;
        } else {
            pthread_mutex_lock(&f->mtx);
            f->fd = -1;
            close(s);
            pthread_mutex_unlock(&f->mtx);
        }
    }
    freeaddrinfo(list);
    if (fd < 0) {
        if (timed_out && last_err == ETIMEDOUT) {
            snprintf(why, sizeof why, "no connection to %s port %s within %u s", f->host, f->port, f->timeout);
            fail(f, WF_E_TIMEOUT, why);
        } else {
            snprintf(why, sizeof why, "%s port %s: %s", f->host, f->port, strerror(last_err ? last_err : ECONNREFUSED));
            fail(f, WF_E_CONNECT, why);
        }
    }
    return fd;
}

static int is_ip(const char *host)
{
    struct in6_addr a6;
    struct in_addr a4;
    return inet_pton(AF_INET, host, &a4) == 1 || inet_pton(AF_INET6, host, &a6) == 1;
}

/* The TLS handshake and the certificate's check: 0, or -1 (the fetch says why). */
static int handshake(struct fetch *f, struct conn *c)
{
    X509 *cert;
    long v;
    char why[400];
    pthread_once(&tls_once, tls_init);
    if (!tls_ctx || !(c->ssl = SSL_new(tls_ctx))) {
        fail(f, WF_E_HOST, "no TLS context (the system's trust store did not load)");
        return -1;
    }
    SSL_set_fd(c->ssl, c->fd);
    if (is_ip(f->host))
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), f->host);
    else {
        SSL_set_tlsext_host_name(c->ssl, f->host);
        SSL_set1_host(c->ssl, f->host);
    }
    ERR_clear_error();
    errno = 0;
    if ((v = SSL_connect(c->ssl)) != 1) {
        int se = errno, e = SSL_get_error(c->ssl, (int)v);
        unsigned long q = ERR_peek_last_error();
        if (is_cancelled(f))
            fail(f, WF_E_CANCELLED, "cancelled");
        else if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE || se == EAGAIN || se == EWOULDBLOCK) {
            snprintf(why, sizeof why, "no TLS handshake within %u s", f->timeout);
            fail(f, WF_E_TIMEOUT, why);
        } else {
            snprintf(why, sizeof why, "TLS handshake failed: %s",
                     q && ERR_reason_error_string(q) ? ERR_reason_error_string(q) : "the connection closed");
            fail(f, WF_E_TLS, why);
        }
        return -1;
    }
    v = SSL_get_verify_result(c->ssl);
    cert = SSL_get1_peer_certificate(c->ssl);
    pthread_mutex_lock(&f->mtx);
    if (v != X509_V_OK || !cert) {
        size_t n = strlen(f->info);
        if (!f->error) {
            f->error = f->cancelled ? WF_E_CANCELLED : WF_E_TLS;
            snprintf(f->info + n, sizeof f->info - n, "error=%s\n",
                     f->cancelled ? "cancelled" : !cert ? "the server sent no certificate" : X509_verify_cert_error_string(v));
            summarise_locked(f, cert);
        }
        f->body_done = 1;
        changed_locked(f);
        pthread_mutex_unlock(&f->mtx);
        X509_free(cert);
        return -1;
    }
    {
        size_t n0 = strlen(f->info), n = n0;
        snprintf(f->info + n, sizeof f->info - n, "tls=%s\ncipher=%s\n", SSL_get_version(c->ssl), SSL_get_cipher_name(c->ssl));
        summarise_locked(f, cert);
        n = strlen(f->info);
        snprintf(f->info + n, sizeof f->info - n, "verified=yes\n");
        snprintf(c->tls, sizeof c->tls, "%s", f->info + n0);   /* given again with each fetch on a kept connection */
    }
    pthread_mutex_unlock(&f->mtx);
    X509_free(cert);
    return 0;
}

/* ---- the request ------------------------------------------------------------------- */

/* The next part of the body to send: 1 with *p (the caller frees it), 0 at
 * the end, -1 cancelled, -2 nothing came within the timeout. */
static int next_piece(struct fetch *f, struct piece **p)
{
    struct timespec until = deadline_ms(f->timeout * 1000);
    int r;
    pthread_mutex_lock(&f->mtx);
    for (;;) {
        if (f->cancelled) {
            r = -1;
            break;
        }
        if (f->up_head) {
            *p = f->up_head;
            f->up_head = (*p)->next;
            if (!f->up_head)
                f->up_tail = NULL;
            f->up_bytes -= (*p)->length;
            pthread_cond_broadcast(&f->cv);     /* room for SEND */
            r = 1;
            break;
        }
        if (f->up_last) {
            r = 0;
            break;
        }
        if (pthread_cond_timedwait(&f->cv, &f->mtx, &until) == ETIMEDOUT && !f->up_head && !f->up_last && !f->cancelled) {
            r = -2;
            break;
        }
    }
    pthread_mutex_unlock(&f->mtx);
    return r;
}

/* No more of the body is taken: later SENDs are dropped. */
static void upload_closed(struct fetch *f)
{
    pthread_mutex_lock(&f->mtx);
    f->up_closed = 1;
    while (f->up_head) {
        struct piece *p = f->up_head;
        f->up_head = p->next;
        free(p);
    }
    f->up_tail = NULL;
    f->up_bytes = 0;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mtx);
}

/* A kept connection failed before a byte of the answer came (not a
 * timeout, not a cancel): the server closed it while it was idle, so the
 * request goes again on a new one. */
static int stale(struct fetch *f, struct conn *c)
{
    return c->reused && !c->timedout && !c->heard && !is_cancelled(f);
}

/* The request's head and its body: 0, 1 when the connection broke during
 * the body (the answer may still be there), -1 when the fetch failed, -3
 * when a kept connection turned out closed (send it again on a new one). */
static int send_request(struct fetch *f, struct conn *c)
{
    int chunked = f->body_length == WF_LENGTH_UNKNOWN, r = 0;
    if (conn_write(c, f->request, strlen(f->request))) {
        if (stale(f, c))
            return -3;
        conn_fail(f, c, WF_E_CLOSED, "while sending the request");
        return -1;
    }
    if (!(f->flags & WF_BODY))
        return 0;
    for (;;) {
        struct piece *p = NULL;
        int k = next_piece(f, &p);
        if (k == 1) {
            char size[16];
            int bad = 0;
            if (chunked && p->length) {
                snprintf(size, sizeof size, "%x\r\n", (unsigned)p->length);
                bad = conn_write(c, size, strlen(size)) || conn_write(c, p->data, p->length) || conn_write(c, "\r\n", 2);
            } else if (p->length)
                bad = conn_write(c, p->data, p->length);
            free(p);
            if (bad) {
                r = c->timedout ? -1 : 1;
                if (c->timedout)
                    conn_fail(f, c, WF_E_CLOSED, "while sending the body");
                break;
            }
        } else if (k == 0) {
            if (chunked && conn_write(c, "0\r\n\r\n", 5))
                r = 1;
            break;
        } else if (k == -1) {
            fail(f, WF_E_CANCELLED, "cancelled");
            r = -1;
            break;
        } else {
            char why[100];
            snprintf(why, sizeof why, "the rest of the body to send did not come within %u s", f->timeout);
            fail(f, WF_E_TIMEOUT, why);
            r = -1;
            break;
        }
    }
    upload_closed(f);
    return r;
}

/* ---- the answer ---------------------------------------------------------------------- */

enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_CLOSE };
enum { DECODE_NONE, DECODE_ZLIB, DECODE_BROTLI };

static const char *value_of(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':')
        return NULL;
    line += n + 1;
    while (*line == ' ' || *line == '\t')
        line++;
    return line;
}

/* A coding, trimmed and lower case, into out; 0 when it names more than one. */
static int one_token(const char *v, char *out, size_t room)
{
    size_t n = 0;
    while (*v == ' ' || *v == '\t')
        v++;
    while (*v && *v != ',' && *v != ' ' && *v != '\t' && *v != ';' && n + 1 < room)
        out[n++] = (char)(*v >= 'A' && *v <= 'Z' ? *v + 32 : *v), v++;
    out[n] = 0;
    while (*v == ' ' || *v == '\t')
        v++;
    return *v == 0;
}

/* 1 when the comma-separated list v holds token tok (any case). */
static int has_token(const char *v, const char *tok)
{
    size_t n = strlen(tok);
    while (*v) {
        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;
        if (!strncasecmp(v, tok, n) && (!v[n] || v[n] == ',' || v[n] == ' ' || v[n] == '\t' || v[n] == ';'))
            return 1;
        while (*v && *v != ',')
            v++;
    }
    return 0;
}

/* Reads the head (after any 1xx), and publishes it: the body's framing in
 * *mode and *length, whether the connection may be kept after the body in
 * *keep and for how long idle in *idle; or -1 when the fetch failed, or -3
 * when a kept connection turned out closed before a byte came. */
static int read_head(struct fetch *f, struct conn *c, int broke, int *mode, uint64_t *length, int *decode, int *keep, int *idle)
{
    char *line = malloc(MAX_TEXT), *raw = malloc(MAX_TEXT + 2), *text = NULL, coding[32] = "";
    uint32_t status = 0, version = 11, used, tlen = 0;
    uint64_t clen = 0;
    int have_len = 0, chunked = 0, r, bad = 0, rc = -1, close_it = 0, keep_alive = 0, ka_timeout = 0;
    const char *why = NULL;

    if (!line || !raw) {
        free(line);
        free(raw);
        fail(f, WF_E_HOST, "no memory for the head");
        return -1;
    }
    for (;;) {                                  /* a 1xx head is read and dropped */
        used = 0;
        have_len = chunked = 0;
        close_it = keep_alive = ka_timeout = 0;
        coding[0] = 0;
        r = conn_line(c, line, MAX_TEXT);
        if (r == -2) {
            why = "the answer's head is over 64 KB";
            break;
        }
        if (r <= 0) {
            if (stale(f, c)) {
                rc = -3;
                goto out;
            }
            if (r == 0 && !c->timedout)
                fail(f, WF_E_CLOSED, broke ? "the connection closed while sending the body" : "the connection closed before the answer");
            else
                conn_fail(f, c, WF_E_CLOSED, "before the answer");
            goto out;
        }
        if (strncmp(line, "HTTP/1.", 7) || (line[7] != '0' && line[7] != '1') || line[8] != ' '
            || line[9] < '1' || line[9] > '9' || line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9'
            || (line[12] && line[12] != ' ')) {
            why = "the answer is not HTTP/1.x";
            break;
        }
        version = line[7] == '0' ? 10 : 11;
        status = (uint32_t)((line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0'));
        used = (uint32_t)snprintf(raw, MAX_TEXT, "%s\n", line);
        for (;;) {
            const char *v;
            r = conn_line(c, line, MAX_TEXT);
            if (r == -2) {
                why = "the answer's head is over 64 KB";
                break;
            }
            if (r <= 0) {
                if (r == 0 && !c->timedout)
                    fail(f, WF_E_CLOSED, "the connection closed in the middle of the head");
                else
                    conn_fail(f, c, WF_E_CLOSED, "in the middle of the head");
                goto out;
            }
            if (!line[0])
                break;
            if (used + strlen(line) + 2 > MAX_TEXT) {
                why = "the answer's head is over 64 KB";
                break;
            }
            used += (uint32_t)snprintf(raw + used, MAX_TEXT + 2 - used, "%s\n", line);
            if ((v = value_of(line, "content-length"))) {
                char *end;
                unsigned long long n = strtoull(v, &end, 10);
                while (*end == ' ' || *end == '\t')
                    end++;
                if (*v < '0' || *v > '9' || *end || (have_len && n != clen))
                    bad = 1;
                clen = n;
                have_len = 1;
            } else if ((v = value_of(line, "transfer-encoding"))) {
                const char *last = strrchr(v, ',');
                char tok[32];
                one_token(last ? last + 1 : v, tok, sizeof tok);
                chunked = !strcmp(tok, "chunked");
                if (!chunked && strcmp(tok, "identity"))
                    bad = 1;
            } else if ((v = value_of(line, "content-encoding"))) {
                if (!one_token(v, coding, sizeof coding))
                    strcpy(coding, "?");        /* two codings: left as they are */
            } else if ((v = value_of(line, "connection"))) {
                close_it |= has_token(v, "close");
                keep_alive |= has_token(v, "keep-alive");
            } else if ((v = value_of(line, "keep-alive"))) {
                const char *t = strcasestr(v, "timeout=");
                if (t)
                    ka_timeout = atoi(t + 8);
            }
        }
        if (why)
            break;
        if (status >= 100 && status < 200 && status != 101)
            continue;
        if (status == 101)
            why = "the server switched protocols, which this service does not do";
        else if (bad && !chunked)
            why = "the answer's Content-Length or Transfer-Encoding is not valid";
        break;
    }
    if (why) {
        fail(f, WF_E_PROTOCOL, why);
        goto out;
    }

    if (!strcmp(f->method, "HEAD") || status == 204 || status == 304)
        *mode = BODY_NONE;
    else if (chunked)
        *mode = BODY_CHUNKED;
    else if (have_len)
        *mode = BODY_LENGTH;
    else
        *mode = BODY_CLOSE;
    *length = clen;
    *decode = 0;
    if (*mode != BODY_NONE && (f->flags & WF_DECODE)) {
        if (!strcmp(coding, "gzip") || !strcmp(coding, "x-gzip") || !strcmp(coding, "deflate"))
            *decode = DECODE_ZLIB;
#ifdef WF_BROTLI
        else if (!strcmp(coding, "br"))
            *decode = DECODE_BROTLI;
#endif
    }

    /* The Amiga's copy: no Transfer-Encoding (the body comes de-chunked),
     * and when decoded, no Content-Encoding or Content-Length either. */
    if (!(text = malloc(used + 1))) {
        fail(f, WF_E_HOST, "no memory for the head");
        goto out;
    }
    {
        char *p = raw, *nl;
        while ((nl = strchr(p, '\n'))) {
            size_t n = (size_t)(nl - p) + 1;
            *nl = 0;
            if (!(p != raw && (value_of(p, "transfer-encoding") || (*decode && (value_of(p, "content-encoding")
                                                                              || value_of(p, "content-length")))))) {
                memcpy(text + tlen, p, n - 1);
                text[tlen + n - 1] = '\n';
                tlen += (uint32_t)n;
            }
            p = nl + 1;
        }
        text[tlen] = 0;
    }
    pthread_mutex_lock(&f->mtx);
    f->status = status;
    f->version = version;
    f->htext = text;
    f->hlen = tlen;
    text = NULL;
    f->hflags = (f->tls ? WF_H_TLS | WF_H_VERIFIED : 0) | (*decode ? WF_H_DECODED : 0);
    if (*mode == BODY_NONE) {                   /* body_done is the thread's, once the connection is kept or closed */
        f->hflags |= WF_H_NOBODY | WF_H_LENGTH;
        f->length = 0;
    } else if (*mode == BODY_LENGTH && !*decode) {
        f->hflags |= WF_H_LENGTH;
        f->length = clen;
    }
    f->decoded = *decode;
    if (*mode != BODY_NONE && !(f->ring = malloc(WINDOW))) {
        pthread_mutex_unlock(&f->mtx);
        fail(f, WF_E_HOST, "no memory for the body's window");
        goto out;
    }
    f->head_ready = 1;
    changed_locked(f);
    pthread_mutex_unlock(&f->mtx);
    /* Kept after the body: HTTP/1.1 unless either side said close (1.0 only
     * when the server said keep-alive), and a body framed by its length or
     * chunks; idle a second less than the server's Keep-Alive timeout. */
    *keep = !close_it && (version == 11 || keep_alive) && *mode != BODY_CLOSE;
    *idle = ka_timeout > 0 && ka_timeout <= KEEP_IDLE ? (ka_timeout > 1 ? ka_timeout - 1 : 1) : KEEP_IDLE;
    free(line);
    free(raw);
    return 0;
out:
    free(text);
    free(line);
    free(raw);
    return rc;
}

/* ---- the body ------------------------------------------------------------------------ */

/* Bytes into the window, waiting while it is full: 0, or -1 when cancelled. */
static int push(struct fetch *f, const uint8_t *d, uint32_t n)
{
    pthread_mutex_lock(&f->mtx);
    while (n) {
        uint32_t room, end, k, first;
        while (f->r_len == WINDOW && !f->cancelled)
            pthread_cond_wait(&f->cv, &f->mtx);
        if (f->cancelled) {
            pthread_mutex_unlock(&f->mtx);
            return -1;
        }
        room = WINDOW - f->r_len;
        end = (f->r_at + f->r_len) % WINDOW;
        k = n < room ? n : room;
        first = k < WINDOW - end ? k : WINDOW - end;
        memcpy(f->ring + end, d, first);
        memcpy(f->ring, d + first, k - first);
        f->r_len += k;
        f->body_total += k;
        d += k;
        n -= k;
        changed_locked(f);
    }
    pthread_mutex_unlock(&f->mtx);
    return 0;
}

/* Where body bytes go: the window, through inflate (or Brotli) when decoding. */
struct sink {
    struct fetch *f;
    int decode, started, ended, raw;
    z_stream z;
#ifdef WF_BROTLI
    BrotliDecoderState *br;
#endif
    uint8_t out[IOBUF];
};

/* 0, -1 cancelled, -2 the compressed data is bad. */
static int sink_put(struct sink *s, const uint8_t *d, uint32_t n)
{
    if (!s->decode)
        return push(s->f, d, n);
#ifdef WF_BROTLI
    if (s->decode == DECODE_BROTLI) {
        size_t in_left = n;
        const uint8_t *next_in = d;
        if (!s->started && !(s->br = BrotliDecoderCreateInstance(NULL, NULL, NULL)))
            return -2;
        s->started = 1;
        while (!s->ended) {
            size_t out_left = sizeof s->out;
            uint8_t *next_out = s->out;
            BrotliDecoderResult r = BrotliDecoderDecompressStream(s->br, &in_left, &next_in, &out_left, &next_out, NULL);
            uint32_t got = (uint32_t)(sizeof s->out - out_left);
            if (got && push(s->f, s->out, got))
                return -1;
            if (r == BROTLI_DECODER_RESULT_ERROR)
                return -2;
            if (r == BROTLI_DECODER_RESULT_SUCCESS)
                s->ended = 1;                   /* anything after the end is dropped */
            else if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT)
                break;
        }
        return 0;
    }
#endif
    if (!s->started) {
        /* gzip and zlib by their headers; "deflate" without zlib's header is raw. */
        int bits = 15 + 32;
        if (n >= 2 && d[0] != 0x1f && ((d[0] & 0x0f) != 8 || ((d[0] << 8) | d[1]) % 31))
            bits = -15;
        if (inflateInit2(&s->z, bits) != Z_OK)
            return -2;
        s->started = 1;
        s->raw = bits < 0;
    }
    s->z.next_in = (Bytef *)d;
    s->z.avail_in = n;
    while (s->z.avail_in) {
        int r;
        uint32_t got;
        if (s->ended) {
            /* Another gzip member may follow; anything else after the end is dropped. */
            if (s->raw || s->z.next_in[0] != 0x1f)
                return 0;
            inflateReset(&s->z);
            s->ended = 0;
        }
        s->z.next_out = s->out;
        s->z.avail_out = sizeof s->out;
        r = inflate(&s->z, Z_NO_FLUSH);
        got = (uint32_t)(sizeof s->out - s->z.avail_out);
        if (got && push(s->f, s->out, got))
            return -1;
        if (r == Z_STREAM_END)
            s->ended = 1;
        else if (r != Z_OK && !(r == Z_BUF_ERROR && got))
            return -2;
    }
    return 0;
}

static void sink_end(struct sink *s)
{
#ifdef WF_BROTLI
    if (s->br) {
        BrotliDecoderDestroyInstance(s->br);
        return;
    }
#endif
    if (s->started)
        inflateEnd(&s->z);
}

/* n bytes of body (n == UINT64_MAX: up to the connection's end) into the sink: 0, or -1 (failed). */
static int body_bytes(struct fetch *f, struct conn *c, struct sink *s, uint64_t n)
{
    int to_end = n == UINT64_MAX;
    while (n) {
        uint32_t k;
        int r;
        if (c->pos == c->len) {
            r = conn_fill(c);
            if (r == 0 && to_end)
                return 0;
            if (r == 0) {
                char why[120];
                snprintf(why, sizeof why, "the connection closed with %llu bytes of the body still to come", (unsigned long long)n);
                fail(f, WF_E_CLOSED, why);
                return -1;
            }
            if (r < 0) {
                conn_fail(f, c, WF_E_CLOSED, "in the middle of the body");
                return -1;
            }
        }
        k = c->len - c->pos;
        if (!to_end && k > n)
            k = (uint32_t)n;
        r = sink_put(s, c->buf + c->pos, k);
        if (r == -1) {
            fail(f, WF_E_CANCELLED, "cancelled");
            return -1;
        }
        if (r == -2) {
            fail(f, WF_E_DECODE, "the compressed body is not valid gzip or deflate data");
            return -1;
        }
        c->pos += k;
        if (!to_end)
            n -= k;
    }
    return 0;
}

static int read_body(struct fetch *f, struct conn *c, int mode, uint64_t length, int decode)
{
    struct sink *s = calloc(1, sizeof *s);
    int r = 0;
    if (!s) {
        fail(f, WF_E_HOST, "no memory for the body");
        return -1;
    }
    s->f = f;
    s->decode = decode;
    if (mode == BODY_LENGTH)
        r = body_bytes(f, c, s, length);
    else if (mode == BODY_CLOSE)
        r = body_bytes(f, c, s, UINT64_MAX);
    else if (mode == BODY_CHUNKED) {
        char line[1024];
        for (;;) {
            char *end;
            unsigned long long size;
            int k = conn_line(c, line, sizeof line);
            if (k == -2) {
                fail(f, WF_E_PROTOCOL, "a chunk's size line is over 1 KB");
                r = -1;
                break;
            }
            if (k <= 0) {
                if (k == 0 && !c->timedout)
                    fail(f, WF_E_CLOSED, "the connection closed in the middle of the chunked body");
                else
                    conn_fail(f, c, WF_E_CLOSED, "in the middle of the chunked body");
                r = -1;
                break;
            }
            size = strtoull(line, &end, 16);
            if (end == line || (*end && *end != ';' && *end != ' ' && *end != '\t')) {
                fail(f, WF_E_PROTOCOL, "a chunk's size is not valid");
                r = -1;
                break;
            }
            if (!size) {                        /* the trailers, up to an empty line or the end */
                while ((k = conn_line(c, line, sizeof line)) > 0 && line[0])
                    ;
                if (k <= 0)
                    c->spent = 1;               /* the body is whole, but the connection is not fit to keep */
                break;
            }
            if ((r = body_bytes(f, c, s, size)))
                break;
            if ((k = conn_line(c, line, sizeof line)) <= 0 || line[0]) {
                if (k <= 0 && c->timedout)
                    conn_fail(f, c, WF_E_CLOSED, "in the middle of the chunked body");
                else
                    fail(f, k <= 0 ? WF_E_CLOSED : WF_E_PROTOCOL, k <= 0 ? "the connection closed in the middle of the chunked body"
                                                                        : "a chunk does not end where its size says");
                r = -1;
                break;
            }
        }
    }
    if (!r && decode && s->started && !s->ended) {
        fail(f, WF_E_DECODE, "the compressed body ended early");
        r = -1;
    }
    sink_end(s);
    free(s);
    return r;
}

/* ---- kept connections -------------------------------------------------------------------- */

struct kept {
    struct kept *next;
    int tls, fd;
    SSL *ssl;
    char host[256], port[8];
    struct timespec until;      /* closed if still idle then */
    char info[INFO_ROOM];       /* the info lines of its handshake */
};

static pthread_mutex_t pool_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct kept *pool;       /* the newest first */
static int pool_n;
static pthread_once_t keep_once = PTHREAD_ONCE_INIT;
static int keep_on;

static void keep_init(void)
{
    const char *v = getenv("OPENWEB_FETCH_KEEP");
    keep_on = !(v && !strcmp(v, "0"));
}

/* Connections are kept unless OPENWEB_FETCH_KEEP=0 (then v1's "Connection: close"). */
static int keeping(void)
{
    pthread_once(&keep_once, keep_init);
    return keep_on;
}

static void kept_close(struct kept *k)
{
    if (k->ssl) {
        fcntl(k->fd, F_SETFL, fcntl(k->fd, F_GETFL) | O_NONBLOCK);
        SSL_shutdown(k->ssl);                   /* close_notify, if the socket takes it now */
        SSL_free(k->ssl);
        ERR_clear_error();
    }
    close(k->fd);
    free(k);
}

/* 1 when a kept connection still looks open: nothing to read on it, or (TLS)
 * only records that carry no data, such as a late session ticket. */
static int kept_alive(struct kept *k)
{
    struct pollfd p = { k->fd, POLLIN, 0 };
    int r = poll(&p, 1, 0), alive;
    char b;
    if (r == 0)
        return 1;
    if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return 0;
    if (!k->ssl) {
        ssize_t n = recv(k->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    fcntl(k->fd, F_SETFL, fcntl(k->fd, F_GETFL) | O_NONBLOCK);
    ERR_clear_error();
    r = SSL_peek(k->ssl, &b, 1);
    alive = r <= 0 && SSL_get_error(k->ssl, r) == SSL_ERROR_WANT_READ;
    ERR_clear_error();
    fcntl(k->fd, F_SETFL, fcntl(k->fd, F_GETFL) & ~O_NONBLOCK);
    return alive;
}

/* A kept connection to f's scheme, host and port into c, ready for the
 * request: 1, or 0 when there is none (or the fetch was cancelled). */
static int pool_take(struct fetch *f, struct conn *c)
{
    for (;;) {
        struct kept *k = NULL, **p;
        struct timeval tv = { (time_t)f->timeout, 0 };
        size_t n;
        pthread_mutex_lock(&pool_mtx);
        for (p = &pool; *p; p = &(*p)->next)
            if ((*p)->tls == f->tls && !strcmp((*p)->host, f->host) && !strcmp((*p)->port, f->port)) {
                k = *p;
                *p = k->next;
                pool_n--;
                break;
            }
        pthread_mutex_unlock(&pool_mtx);
        if (!k)
            return 0;
        if (!kept_alive(k)) {                   /* the server closed it while it was idle */
            kept_close(k);
            continue;
        }
        pthread_mutex_lock(&f->mtx);
        if (f->cancelled) {
            pthread_mutex_unlock(&f->mtx);
            kept_close(k);
            return 0;
        }
        f->fd = k->fd;                          /* so CANCEL can shut it */
        n = strlen(f->info);
        snprintf(f->info + n, sizeof f->info - n, "%sreused=yes\n", k->info);
        pthread_mutex_unlock(&f->mtx);
        setsockopt(k->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(k->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        c->fd = k->fd;
        c->ssl = k->ssl;
        snprintf(c->tls, sizeof c->tls, "%s", k->info);
        c->reused = 1;
        c->heard = c->spent = c->timedout = 0;
        c->pos = c->len = 0;
        free(k);
        return 1;
    }
}

/* c's connection into the pool, to be closed after idle seconds unused: 1,
 * and c no longer has it; 0 when the fetch was cancelled (c keeps it). */
static int pool_put(struct fetch *f, struct conn *c, int idle)
{
    struct kept *k = calloc(1, sizeof *k), *gone = NULL, **p, **last = NULL, **last_same = NULL;
    struct timespec until;
    int same = 0;
    if (!k)
        return 0;
    pthread_mutex_lock(&f->mtx);
    if (f->cancelled || f->error) {
        pthread_mutex_unlock(&f->mtx);
        free(k);
        return 0;
    }
    f->fd = -1;                                 /* a late CANCEL no longer reaches it */
    pthread_mutex_unlock(&f->mtx);
    k->tls = f->tls;
    k->fd = c->fd;
    k->ssl = c->ssl;
    snprintf(k->host, sizeof k->host, "%s", f->host);
    snprintf(k->port, sizeof k->port, "%s", f->port);
    snprintf(k->info, sizeof k->info, "%s", c->tls);
    k->until = until = deadline_ms((uint32_t)idle * 1000);
    c->fd = -1;
    c->ssl = NULL;
    pthread_mutex_lock(&pool_mtx);
    for (p = &pool; *p; p = &(*p)->next) {
        if ((*p)->tls == k->tls && !strcmp((*p)->host, k->host) && !strcmp((*p)->port, k->port)) {
            same++;
            last_same = p;
        }
        last = p;
    }
    if (same >= KEEP_PER_HOST) {                /* the oldest to the same place goes */
        gone = *last_same;
        *last_same = gone->next;
        pool_n--;
    } else if (pool_n >= KEEP_MAX) {            /* the oldest of all goes */
        gone = *last;
        *last = NULL;
        pool_n--;
    }
    k->next = pool;
    pool = k;
    pool_n++;
    pthread_mutex_unlock(&pool_mtx);
    if (gone)
        kept_close(gone);
    timer_poke_at(&until);
    return 1;
}

/* Under no lock, from the timer: kept connections idle past their time are
 * closed; *next comes forward to the next one's. */
static void pool_expire(const struct timespec *now, struct timespec *next)
{
    struct kept *gone = NULL, **p, *k;
    pthread_mutex_lock(&pool_mtx);
    for (p = &pool; (k = *p); ) {
        if (!ts_before(now, &k->until)) {
            *p = k->next;
            pool_n--;
            k->next = gone;
            gone = k;
            continue;
        }
        if (ts_before(&k->until, next))
            *next = k->until;
        p = &k->next;
    }
    pthread_mutex_unlock(&pool_mtx);
    while ((k = gone)) {
        gone = k->next;
        kept_close(k);
    }
}

/* A kept connection that turned out closed before a byte of the answer:
 * it goes, and the fetch starts again on a new one. */
static void drop_stale(struct fetch *f, struct conn *c)
{
    if (c->ssl)
        SSL_free(c->ssl);
    ERR_clear_error();
    pthread_mutex_lock(&f->mtx);
    if (f->fd >= 0)
        close(f->fd);
    f->fd = -1;
    if (!f->error)
        f->info[0] = 0;                         /* the new connection's own lines follow */
    pthread_mutex_unlock(&f->mtx);
    c->fd = -1;
    c->ssl = NULL;
    c->reused = c->heard = c->spent = c->timedout = 0;
    c->pos = c->len = 0;
    c->tls[0] = 0;
}

/* ---- the fetch's thread ---------------------------------------------------------------- */

static void run(struct fetch *f, struct conn *c)
{
    int mode = BODY_NONE, decode = 0, broke = 0, keep = 0, idle = KEEP_IDLE, r = 0;
    uint64_t length = 0;
    /* A request with a body never goes on a kept connection: if the server
     * had closed it, the body could not be sent again. */
    int take = keeping() && !(f->flags & WF_BODY);

    for (;;) {
        if (!(take && pool_take(f, c))) {
            if ((c->fd = dial(f)) < 0)
                return;
            if (f->tls && handshake(f, c))
                return;
        }
        take = 0;
        broke = send_request(f, c);
        if (broke >= 0)
            r = read_head(f, c, broke, &mode, &length, &decode, &keep, &idle);
        if (broke == -3 || r == -3) {           /* a kept connection the server had closed: once more, on a new one */
            drop_stale(f, c);
            r = 0;
            continue;
        }
        if (broke < 0 || r)
            return;
        break;
    }
    if (mode != BODY_NONE && read_body(f, c, mode, length, decode))
        return;
    if (keep && keeping() && timer_ready() && !broke && !c->spent && c->pos == c->len && !(c->ssl && SSL_pending(c->ssl)))
        pool_put(f, c, idle);
    pthread_mutex_lock(&f->mtx);
    f->body_done = 1;
    changed_locked(f);
    pthread_mutex_unlock(&f->mtx);
}

static void *fetch_thread(void *arg)
{
    struct fetch *f = arg;
    struct conn *c = calloc(1, sizeof *c);
    sigset_t pipe;

    /* A write to a closed connection must not end the emulator. */
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, NULL);
    if (c) {
        c->fd = -1;
        run(f, c);
        if (c->ssl)
            SSL_free(c->ssl);
    } else
        fail(f, WF_E_HOST, "no memory for the connection");
    upload_closed(f);
    pthread_mutex_lock(&f->mtx);
    if (f->fd >= 0)
        close(f->fd);
    f->fd = -1;
    f->running = 0;
    if (!f->body_done) {
        f->body_done = 1;
        if (!f->error)
            f->error = WF_E_CANCELLED;
    }
    changed_locked(f);
    pthread_mutex_unlock(&f->mtx);
    free(c);
    ERR_clear_error();
    pthread_mutex_lock(&table_mtx);
    threads_running--;
    pthread_mutex_unlock(&table_mtx);
    release(f);
    return NULL;
}

/* ---- OPEN: the request text ---------------------------------------------------------------- */

static int tchar(int ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

/* The URL into f (scheme, host, port, target) and the Host header's value: 0, or -1. */
static int parse_url(struct fetch *f, const char *u, char *hosthdr, size_t room)
{
    const char *p, *auth_end, *host, *host_end, *port = NULL, *t;
    size_t hn, pn = 0, i;
    unsigned long portv;

    for (p = u; *p; p++)
        if ((uint8_t)*p <= 0x20 || *p == 0x7f)
            return -1;
    if (!strncasecmp(u, "http://", 7)) {
        f->tls = 0;
        p = u + 7;
    } else if (!strncasecmp(u, "https://", 8)) {
        f->tls = 1;
        p = u + 8;
    } else
        return -1;
    auth_end = p + strcspn(p, "/?#");
    if (memchr(p, '@', (size_t)(auth_end - p)))
        return -1;                              /* no user names in v1 */
    if (*p == '[') {
        host = p + 1;
        if (!(host_end = memchr(host, ']', (size_t)(auth_end - host))))
            return -1;
        if (host_end + 1 < auth_end) {
            if (host_end[1] != ':')
                return -1;
            port = host_end + 2;
        }
        for (t = host; t < host_end; t++)
            if (!((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'f') || (*t >= 'A' && *t <= 'F') || *t == ':' || *t == '.'))
                return -1;
    } else {
        host = p;
        host_end = memchr(p, ':', (size_t)(auth_end - p));
        if (host_end)
            port = host_end + 1;
        else
            host_end = auth_end;
        for (t = host; t < host_end; t++)
            if (!((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || *t == '-' || *t == '.' || *t == '_'))
                return -1;
    }
    hn = (size_t)(host_end - host);
    if (!hn || hn >= sizeof f->host)
        return -1;
    memcpy(f->host, host, hn);
    f->host[hn] = 0;
    if (port) {
        pn = (size_t)(auth_end - port);
        portv = 0;
        for (i = 0; i < pn; i++) {
            if (port[i] < '0' || port[i] > '9' || i >= 5)
                return -1;
            portv = portv * 10 + (unsigned long)(port[i] - '0');
        }
        if (pn && (portv < 1 || portv > 65535))
            return -1;
    }
    if (pn)
        snprintf(f->port, sizeof f->port, "%lu", portv);
    else
        snprintf(f->port, sizeof f->port, "%s", f->tls ? "443" : "80");
    snprintf(hosthdr, room, "%s%s%s", *p == '[' ? "[" : "", f->host, *p == '[' ? "]" : "");
    if (pn && strcmp(f->port, f->tls ? "443" : "80")) {
        size_t n = strlen(hosthdr);
        snprintf(hosthdr + n, room - n, ":%s", f->port);
    }
    /* The target: the path and the query, without the fragment. */
    t = auth_end;
    {
        size_t tn = strcspn(t, "#");
        if (!(f->target = malloc(tn + 2)))
            return -1;
        if (*t == '/')
            snprintf(f->target, tn + 2, "%.*s", (int)tn, t);
        else
            snprintf(f->target, tn + 2, "/%.*s", (int)tn, t);
    }
    return 0;
}

/* Headers the host writes itself: the Amiga's own copies are dropped. */
static int host_writes(const char *name, size_t n, int decode)
{
    static const char *const names[] = { "host", "connection", "content-length", "transfer-encoding", "keep-alive",
                                         "proxy-connection", "te", "upgrade", "trailer", NULL };
    int i;
    for (i = 0; names[i]; i++)
        if (strlen(names[i]) == n && !strncasecmp(name, names[i], n))
            return 1;
    return decode && n == 15 && !strncasecmp(name, "accept-encoding", 15);
}

/* The request text into f->method, the URL and f->request (the whole head to send): 0, or -1. */
static int parse_request(struct fetch *f, const uint8_t *text, uint32_t n)
{
    char *s = malloc((size_t)n + 1), *line, *next, *sp, hosthdr[300];
    size_t room = 2 * (size_t)n + 1024, used;   /* each line may gain ": " and a CR */
    int first = 1, ua = 0, rc = -1, decode = (f->flags & WF_DECODE) != 0;

    if (!s)
        return -1;
    memcpy(s, text, n);
    s[n] = 0;                                   /* length or NUL ended */
    if (!(f->request = malloc(room)))
        goto out;
    used = 0;
    for (line = s; line && *line; line = next) {
        size_t len;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[--len] = 0;
        if (first) {
            size_t m;
            first = 0;
            if (!(sp = strchr(line, ' ')))
                goto out;
            m = (size_t)(sp - line);
            if (!m || m >= sizeof f->method)
                goto out;
            for (size_t i = 0; i < m; i++)
                if (!tchar((uint8_t)line[i]))
                    goto out;
            memcpy(f->method, line, m);
            f->method[m] = 0;
            while (*sp == ' ')
                sp++;
            for (len = strlen(sp); len && sp[len - 1] == ' '; )
                sp[--len] = 0;
            if (parse_url(f, sp, hosthdr, sizeof hosthdr))
                goto out;
            used = (size_t)snprintf(f->request, room, "%s %s HTTP/1.1\r\nHost: %s\r\n", f->method, f->target, hosthdr);
            if (used >= room)
                goto out;
            continue;
        }
        if (!len)
            break;                              /* an empty line ends the headers */
        {
            char *colon = strchr(line, ':'), *v, *e;
            size_t nn;
            if (!colon || colon == line)
                goto out;
            nn = (size_t)(colon - line);
            for (size_t i = 0; i < nn; i++)
                if (!tchar((uint8_t)line[i]))
                    goto out;
            for (v = colon + 1; *v == ' ' || *v == '\t'; v++)
                ;
            for (e = v; *e; e++)
                if (((uint8_t)*e < 0x20 && *e != '\t') || *e == 0x7f)
                    goto out;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = 0;
            if (host_writes(line, nn, decode))
                continue;
            if (nn == 10 && !strncasecmp(line, "user-agent", 10))
                ua = 1;
            if (used + nn + strlen(v) + 5 >= room)
                goto out;
            used += (size_t)snprintf(f->request + used, room - used, "%.*s: %s\r\n", (int)nn, line, v);
        }
    }
    if (first)
        goto out;
    {
        char tail[200];
        int k = snprintf(tail, sizeof tail, "%s%s%s",
                         ua ? "" : "User-Agent: GFOpenWeb-Fetch/1\r\n",
                         decode ? "Accept-Encoding: " ACCEPT_ENCODING "\r\n" : "",
                         keeping() ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
        if (f->flags & WF_BODY) {
            if (f->body_length == WF_LENGTH_UNKNOWN)
                k += snprintf(tail + k, sizeof tail - (size_t)k, "Transfer-Encoding: chunked\r\n");
            else
                k += snprintf(tail + k, sizeof tail - (size_t)k, "Content-Length: %llu\r\n", (unsigned long long)f->body_length);
        }
        if (used + (size_t)k + 3 >= room)
            goto out;
        used += (size_t)snprintf(f->request + used, room - used, "%s\r\n", tail);
    }
    rc = 0;
out:
    free(s);
    return rc;
}

/* ---- the calls ------------------------------------------------------------------------ */

static struct fetch *fetch_new(void)
{
    struct fetch *f = calloc(1, sizeof *f);
    pthread_condattr_t ca;
    if (!f)
        return NULL;
    pthread_mutex_init(&f->mtx, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&f->cv, &ca);
    pthread_condattr_destroy(&ca);
    f->fd = -1;
    return f;
}

/* Under table_mtx: fetches the Amiga has not touched for IDLE_CLOSE seconds
 * (a program that ended without CLOSE, a reset) leave their slots; their
 * references go into gone[] for the caller to drop. */
static int reap_locked(struct fetch **gone)
{
    time_t now = now_s();
    int i, n = 0;
    for (i = 0; i < MAX_FETCHES; i++) {
        struct fetch *f = slots[i].f;
        int idle;
        if (!f)
            continue;
        pthread_mutex_lock(&f->mtx);
        idle = now - f->touched > IDLE_CLOSE;
        pthread_mutex_unlock(&f->mtx);
        if (idle) {
            slots[i].f = NULL;
            gone[n++] = f;
        }
    }
    return n;
}

static void drop(struct fetch *f)
{
    pthread_mutex_lock(&f->mtx);
    cancel_locked(f);
    pthread_mutex_unlock(&f->mtx);
    release(f);
}

static int op_open(const uint32_t x[4], struct wf_buffer *buf, uint32_t *result)
{
    struct fetch *f, *gone[MAX_FETCHES];
    pthread_attr_t attr;
    pthread_t t;
    int i, n, slot = -1, rc;
    uint32_t gen = 0;

    if (!buf[0].in || !buf[0].length || buf[0].length > MAX_TEXT || (x[0] & ~(WF_BODY | WF_DECODE)))
        return WF_BADREQUEST;
    if (!(x[0] & WF_BODY) && buf[1].length)
        return WF_BADREQUEST;                   /* a body without WF_BODY */
    if ((x[0] & WF_BODY) && buf[1].length && x[1] != WF_LENGTH_UNKNOWN && buf[1].length > x[1])
        return WF_BADREQUEST;
    if (!(f = fetch_new()))
        return WF_HOSTERROR;
    f->flags = x[0];
    f->timeout = !x[2] ? TIMEOUT_DEF : x[2] > TIMEOUT_MAX ? TIMEOUT_MAX : x[2];
    f->body_length = (x[0] & WF_BODY) ? (x[1] == WF_LENGTH_UNKNOWN ? WF_LENGTH_UNKNOWN : x[1]) : 0;
    if (parse_request(f, buf[0].in, buf[0].length)) {
        fetch_free(f);
        return WF_BADREQUEST;
    }
    if ((x[0] & WF_BODY) && buf[1].in && buf[1].length) {
        struct piece *p = malloc(sizeof *p + buf[1].length);
        if (!p) {
            fetch_free(f);
            return WF_HOSTERROR;
        }
        p->next = NULL;
        p->length = buf[1].length;
        memcpy(p->data, buf[1].in, buf[1].length);
        f->up_head = f->up_tail = p;
        f->up_bytes = p->length;
        f->up_taken = p->length;
    }
    if ((x[0] & WF_BODY) && f->body_length != WF_LENGTH_UNKNOWN && f->up_taken == f->body_length)
        f->up_last = 1;
    f->touched = now_s();
    f->running = 1;

    pthread_mutex_lock(&table_mtx);
    n = reap_locked(gone);
    for (i = 0; i < MAX_FETCHES && slot < 0; i++)
        if (!slots[i].f)
            slot = i;
    if (slot >= 0) {
        gen = (slots[slot].gen + 1) & 0xffffff;
        if (!gen)
            gen = 1;
        slots[slot].gen = gen;
        slots[slot].f = f;
        f->refs = 2;                            /* the slot and the thread */
        threads_running++;
    }
    pthread_mutex_unlock(&table_mtx);
    for (i = 0; i < n; i++)
        drop(gone[i]);
    if (slot < 0) {
        fetch_free(f);
        return WF_HOSTERROR;                    /* 64 open already */
    }
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512u << 10);
    rc = pthread_create(&t, &attr, fetch_thread, f);
    pthread_attr_destroy(&attr);
    if (rc) {
        pthread_mutex_lock(&table_mtx);
        slots[slot].f = NULL;
        threads_running--;
        pthread_mutex_unlock(&table_mtx);
        fetch_free(f);
        return WF_HOSTERROR;
    }
    *result = gen << 8 | (uint32_t)(slot + 1);
    return WF_OK;
}

static uint32_t wait_of(uint32_t ms)
{
    return ms > MAX_WAIT_MS ? MAX_WAIT_MS : ms;
}

static int op_send(uint32_t h, const uint32_t x[4], struct wf_buffer *b, uint32_t *result, uint32_t *aux)
{
    struct fetch *f = lookup(h);
    struct timespec until = deadline_ms(wait_of(x[1]));
    uint32_t n = b[0].in ? b[0].length : 0;
    int last = (x[0] & WF_LAST) != 0, st = WF_OK;

    if (!f)
        return WF_BADREQUEST;
    pthread_mutex_lock(&f->mtx);
    if (!(f->flags & WF_BODY))
        st = WF_BADREQUEST;
    else if (f->error || f->up_closed) {
        *result = n;                            /* not wanted any more: taken and dropped */
        *aux = f->error;
    } else if (f->up_last || (f->body_length != WF_LENGTH_UNKNOWN && f->up_taken + n > f->body_length)
               || (last && f->body_length != WF_LENGTH_UNKNOWN && f->up_taken + n < f->body_length))
        st = WF_BADREQUEST;                     /* after the last part, or not the length OPEN gave */
    else {
        while (!(f->up_bytes == 0 || f->up_bytes + n <= UPLOAD_MAX) && !f->error && !f->up_closed
               && pthread_cond_timedwait(&f->cv, &f->mtx, &until) != ETIMEDOUT)
            ;
        if (f->error || f->up_closed) {
            *result = n;
            *aux = f->error;
        } else if (f->up_bytes == 0 || f->up_bytes + n <= UPLOAD_MAX) {
            struct piece *p = n ? malloc(sizeof *p + n) : NULL;
            if (n && !p)
                st = WF_HOSTERROR;
            else {
                if (p) {
                    p->next = NULL;
                    p->length = n;
                    memcpy(p->data, b[0].in, n);
                    if (f->up_tail)
                        f->up_tail->next = p;
                    else
                        f->up_head = p;
                    f->up_tail = p;
                    f->up_bytes += n;
                    f->up_taken += n;
                }
                if (last || (f->body_length != WF_LENGTH_UNKNOWN && f->up_taken == f->body_length))
                    f->up_last = 1;
                pthread_cond_broadcast(&f->cv);
                *result = n;
            }
        }                                       /* else full: result 0, send it again */
    }
    pthread_mutex_unlock(&f->mtx);
    release(f);
    return st;
}

/* Under f->mtx: HEAD has its answer (the head, or the error). */
static int head_ready_locked(struct fetch *f)
{
    return f->head_ready || f->error;
}

/* Under f->mtx: READ has its answer (bytes, the end, or a cancel's error). */
static int read_ready_locked(struct fetch *f)
{
    return f->r_len || f->body_done || (f->cancelled && f->error);
}

/* Under f->mtx: HEAD's answer as it stands, into out (room bytes; NULL for
 * none). Not ready: result 0 and aux 0. Returns the status. */
static int head_answer_locked(struct fetch *f, uint8_t *out, uint32_t room, uint32_t *result, uint32_t *aux, uint32_t *written)
{
    uint32_t tlen, ilen, need, flags, error;
    char extra[48] = "";

    *result = *aux = *written = 0;
    if (!head_ready_locked(f))
        return WF_OK;                           /* not yet */
    flags = f->head_ready ? f->hflags : 0;
    error = f->head_ready ? 0 : f->error;
    if (f->head_ready && f->decoded && f->body_done && !f->error) {
        flags |= WF_H_LENGTH;
        snprintf(extra, sizeof extra, "Content-Length: %llu\n", (unsigned long long)f->body_total);
    }
    tlen = f->head_ready ? f->hlen + (uint32_t)strlen(extra) : 0;
    ilen = (uint32_t)strlen(f->info);
    need = WF_HEAD_SIZE + tlen + ilen;
    if (!out || room < need) {
        *result = need;
        return WF_TOOSMALL;
    }
    put32(out, WF_HEAD_MAGIC);
    put32(out + 4, error);
    put32(out + 8, f->head_ready ? f->status : 0);
    put32(out + 12, flags);
    put32(out + 16, !(flags & WF_H_LENGTH) ? 0 : f->decoded ? (uint32_t)f->body_total
                    : f->length > 0xffffffffu ? 0xffffffffu : (uint32_t)f->length);
    put32(out + 20, tlen);
    put32(out + 24, ilen);
    put32(out + 28, f->head_ready ? f->version : 0);
    if (tlen) {
        memcpy(out + WF_HEAD_SIZE, f->htext, f->hlen);
        memcpy(out + WF_HEAD_SIZE + f->hlen, extra, strlen(extra));
    }
    memcpy(out + WF_HEAD_SIZE + tlen, f->info, ilen);
    *written = need;
    *result = f->head_ready ? f->status : 0;
    *aux = error;
    return WF_OK;
}

/* Under f->mtx: READ's answer as it stands, into out (room bytes): result
 * the bytes; aux WF_END with the error once nothing more will come. */
static void read_answer_locked(struct fetch *f, uint8_t *out, uint32_t room, uint32_t *result, uint32_t *aux, uint32_t *written)
{
    uint32_t k = 0;
    *aux = 0;
    if (!(f->cancelled && f->error) && f->r_len) {
        uint32_t first;
        k = f->r_len < room ? f->r_len : room;
        first = k < WINDOW - f->r_at ? k : WINDOW - f->r_at;
        memcpy(out, f->ring + f->r_at, first);
        memcpy(out + first, f->ring, k - first);
        f->r_at = (f->r_at + k) % WINDOW;
        f->r_len -= k;
        pthread_cond_broadcast(&f->cv);         /* room in the window */
    }
    *written = k;
    *result = k;
    if (!f->r_len && (f->body_done || (f->cancelled && f->error)))
        *aux = WF_END | (f->error & 0xff) << 8;
}

/* Under f->mtx: the held calls that have their answer now, oldest first, and
 * with now, those whose wait has run out ("not yet", as wf_call answers
 * then), are answered through the card's completion. */
static void serve_held_locked(struct fetch *f, const struct timespec *now)
{
    struct held **p = &f->held;
    while (*p) {
        struct held *h = *p;
        uint32_t result, aux, written;
        int st = WF_OK;
        if (!(h->op == WF_HEAD ? head_ready_locked(f) : read_ready_locked(f)) && !(now && !ts_before(now, &h->until))) {
            p = &h->next;
            continue;
        }
        *p = h->next;
        if (h->op == WF_HEAD)
            st = head_answer_locked(f, h->out, h->room, &result, &aux, &written);
        else
            read_answer_locked(f, h->out, h->room, &result, &aux, &written);
        h->done(h->id, st, result, aux);
        free(h);
    }
}

static int op_head(uint32_t h, const uint32_t x[4], struct wf_buffer *b, uint32_t *result, uint32_t *aux)
{
    struct fetch *f = lookup(h);
    struct timespec until = deadline_ms(wait_of(x[0]));
    int st;

    if (!f)
        return WF_BADREQUEST;
    pthread_mutex_lock(&f->mtx);
    while (!head_ready_locked(f) && pthread_cond_timedwait(&f->cv, &f->mtx, &until) != ETIMEDOUT)
        ;
    st = head_answer_locked(f, b[0].out, b[0].length, result, aux, &b[0].written);
    pthread_mutex_unlock(&f->mtx);
    release(f);
    return st;
}

static int op_read(uint32_t h, const uint32_t x[4], struct wf_buffer *b, uint32_t *result, uint32_t *aux)
{
    struct fetch *f = lookup(h);
    struct timespec until = deadline_ms(wait_of(x[0]));

    if (!f)
        return WF_BADREQUEST;
    if (!b[0].out || !b[0].length) {
        release(f);
        return WF_BADREQUEST;
    }
    pthread_mutex_lock(&f->mtx);
    while (!read_ready_locked(f) && pthread_cond_timedwait(&f->cv, &f->mtx, &until) != ETIMEDOUT)
        ;
    read_answer_locked(f, b[0].out, b[0].length, result, aux, &b[0].written);
    pthread_mutex_unlock(&f->mtx);
    release(f);
    return WF_OK;
}

static int op_cancel(uint32_t h, int close_it)
{
    struct fetch *f;
    if (close_it) {
        uint32_t i = (h & 0xff) - 1;
        pthread_mutex_lock(&table_mtx);
        f = i < MAX_FETCHES && slots[i].gen == h >> 8 ? slots[i].f : NULL;
        if (f)
            slots[i].f = NULL;                  /* the slot's reference is ours to drop */
        pthread_mutex_unlock(&table_mtx);
        if (!f)
            return WF_BADREQUEST;
        drop(f);
        return WF_OK;
    }
    if (!(f = lookup(h)))
        return WF_BADREQUEST;
    drop(f);
    return WF_OK;
}

int wf_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct wf_buffer buf[4],
            uint32_t *result, uint32_t *aux)
{
    static const uint32_t none[4];
    int i;
    *result = *aux = 0;
    for (i = 0; i < 4; i++)
        buf[i].written = 0;
    if (!extra)
        extra = none;
    switch (op) {
    case WF_OPEN: return op_open(extra, buf, result);
    case WF_SEND: return op_send(arg, extra, buf, result, aux);
    case WF_HEAD: return op_head(arg, extra, buf, result, aux);
    case WF_READ: return op_read(arg, extra, buf, result, aux);
    case WF_CANCEL: return op_cancel(arg, 0);
    case WF_CLOSE: return op_cancel(arg, 1);
    case WF_STATS:
        pthread_mutex_lock(&table_mtx);
        for (i = 0; i < MAX_FETCHES; i++)
            if (slots[i].f)
                ++*result;
        *aux = (uint32_t)threads_running;
        pthread_mutex_unlock(&table_mtx);
        return WF_OK;
    }
    return WF_BADREQUEST;
}

/* ---- the timer: held calls whose wait runs out, kept connections idle too long ------------- */

static pthread_mutex_t timer_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cv;
static pthread_once_t timer_once = PTHREAD_ONCE_INIT;
static int timer_up, timer_poked, timer_waiting;
static struct timespec timer_next;      /* when the timer wakes, while timer_waiting */

/* Every fetch in the table, each with a reference for the caller: their count. */
static int gather(struct fetch **fs)
{
    int i, n = 0;
    pthread_mutex_lock(&table_mtx);
    for (i = 0; i < MAX_FETCHES; i++)
        if (slots[i].f) {
            slots[i].f->refs++;
            fs[n++] = slots[i].f;
        }
    pthread_mutex_unlock(&table_mtx);
    return n;
}

static void *timer_thread(void *arg)
{
    sigset_t pipe;
    (void)arg;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, NULL);
    for (;;) {
        struct fetch *fs[MAX_FETCHES];
        struct timespec now, next;
        int i, n;
        clock_gettime(CLOCK_MONOTONIC, &now);
        next = now;
        next.tv_sec += 3600;
        n = gather(fs);
        for (i = 0; i < n; i++) {
            struct held *h;
            pthread_mutex_lock(&fs[i]->mtx);
            if (fs[i]->held)
                serve_held_locked(fs[i], &now);
            for (h = fs[i]->held; h; h = h->next)
                if (ts_before(&h->until, &next))
                    next = h->until;
            pthread_mutex_unlock(&fs[i]->mtx);
            release(fs[i]);
        }
        pool_expire(&now, &next);
        pthread_mutex_lock(&timer_mtx);
        if (!timer_poked) {
            timer_waiting = 1;
            timer_next = next;
            pthread_cond_timedwait(&timer_cv, &timer_mtx, &next);
            timer_waiting = 0;
        }
        timer_poked = 0;
        pthread_mutex_unlock(&timer_mtx);
    }
    return NULL;
}

static void timer_start(void)
{
    pthread_condattr_t ca;
    pthread_attr_t attr;
    pthread_t t;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&timer_cv, &ca);
    pthread_condattr_destroy(&ca);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256u << 10);
    timer_up = !pthread_create(&t, &attr, timer_thread, NULL);
    pthread_attr_destroy(&attr);
}

/* 1 once the timer runs: without it nothing is held or kept. */
static int timer_ready(void)
{
    pthread_once(&timer_once, timer_start);
    return timer_up;
}

/* Something is due at t: the timer wakes for it if it would sleep past it. */
static void timer_poke_at(const struct timespec *t)
{
    if (!timer_ready())
        return;
    pthread_mutex_lock(&timer_mtx);
    if (!timer_waiting || ts_before(t, &timer_next)) {
        timer_poked = 1;
        pthread_cond_signal(&timer_cv);
    }
    pthread_mutex_unlock(&timer_mtx);
}

/* ---- answering later, for the services card ------------------------------------------------- */

int wf_start(uint16_t op, uint32_t arg, const uint32_t extra[4], struct wf_buffer buf[4],
             uint32_t *result, uint32_t *aux, uint32_t id, wf_done_fn done)
{
    static const uint32_t none[4];
    struct fetch *f;
    struct held *h, **p;
    struct timespec until;
    uint32_t wait;
    int i, st = WF_OK;

    if (!extra)
        extra = none;
    wait = wait_of(extra[0]);
    if ((op != WF_HEAD && op != WF_READ) || !wait || !done || !timer_ready())
        return wf_call(op, arg, extra, buf, result, aux);
    *result = *aux = 0;
    for (i = 0; i < 4; i++)
        buf[i].written = 0;
    if (op == WF_READ && (!buf[0].out || !buf[0].length))
        return WF_BADREQUEST;
    if (!(f = lookup(arg)))
        return WF_BADREQUEST;
    pthread_mutex_lock(&f->mtx);
    if (op == WF_HEAD ? head_ready_locked(f) : read_ready_locked(f)) {
        if (op == WF_HEAD)
            st = head_answer_locked(f, buf[0].out, buf[0].length, result, aux, &buf[0].written);
        else
            read_answer_locked(f, buf[0].out, buf[0].length, result, aux, &buf[0].written);
        pthread_mutex_unlock(&f->mtx);
        release(f);
        return st;
    }
    if (!(h = calloc(1, sizeof *h))) {
        pthread_mutex_unlock(&f->mtx);
        release(f);
        return wf_call(op, arg, extra, buf, result, aux);   /* no memory to hold it: it waits here, as before */
    }
    h->id = id;
    h->op = op;
    h->out = buf[0].out;
    h->room = buf[0].length;
    h->until = until = deadline_ms(wait);
    h->done = done;
    for (p = &f->held; *p; p = &(*p)->next)
        ;
    *p = h;                                     /* answered in order, after any held before it */
    pthread_mutex_unlock(&f->mtx);
    release(f);
    timer_poke_at(&until);
    return WF_LATER;
}

void wf_cancel(uint32_t id)
{
    struct fetch *fs[MAX_FETCHES];
    int i, n = gather(fs), found = 0;
    for (i = 0; i < n; i++) {
        if (!found) {
            struct held **p;
            pthread_mutex_lock(&fs[i]->mtx);
            for (p = &fs[i]->held; *p; p = &(*p)->next)
                if ((*p)->id == id) {
                    struct held *h = *p;
                    *p = h->next;
                    h->done(id, WF_CALL_CANCELLED, 0, 0);
                    free(h);
                    found = 1;
                    break;
                }
            pthread_mutex_unlock(&fs[i]->mtx);
        }
        release(fs[i]);
    }
}
