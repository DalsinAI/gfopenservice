/*
 * wfnet on Linux, for its tests (host/test_wfnet.py): the machine side of
 * include/openweb/wfnet.h with POSIX sockets, OpenSSL for TLS (its
 * default trust store, which SSL_CERT_FILE can name) and zlib standing in
 * for the datatypes' decoders (gzip and deflate, when asked for). The
 * protocol code is the Amiga's own, src/wfnet*.c, built unchanged.
 * Built into host/libwfnet.so by build.sh. With WFNP_DATATYPES the
 * decoders are the Amiga's own instead (src/wfnet_dec.c), over
 * stream.datatype's portable core (host/wfnet_dt.c), as host/test_wfnet.py
 * builds it when opendatatypes is there.
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <zlib.h>

#include "openweb/wfnet.h"
#ifdef WFNP_DATATYPES
#include "wfnet_dec.h"
#endif

#define PENDING 64

struct env {
    SSL_CTX *ctx;
    int decoders;
    unsigned char dt_seen;
    struct {                        /* connections under way, with the addresses still to try */
        int fd;
        struct addrinfo *res, *next;
    } pend[PENDING];
};

struct tls {
    SSL *ssl;
    char host[256];
};

static void *p_alloc(void *env, unsigned long n)
{
    (void)env;
    return calloc(1, n);
}

static void p_release(void *env, void *p)
{
    (void)env;
    free(p);
}

static unsigned long p_now(void *env)
{
    struct timespec t;
    (void)env;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 1000UL + (unsigned long)t.tv_nsec / 1000000UL;
}

/* The name's addresses; for the tests, WFNP_HOSTS="name a1 a2..." gives those, in that order. */
static struct addrinfo *resolve(const char *host, const char *ps, const struct addrinfo *hints)
{
    const char *t = getenv("WFNP_HOSTS");
    struct addrinfo *res = NULL, **tail = &res, *one;
    size_t n = strlen(host), k;
    char a[64];
    if (!t || strncmp(t, host, n) || t[n] != ' ')
        return getaddrinfo(host, ps, hints, &res) ? NULL : res;
    for (t += n; *t; t += k) {
        t += strspn(t, " ");
        k = strcspn(t, " ");
        if (k && k < sizeof a) {
            memcpy(a, t, k);
            a[k] = 0;
            if (!getaddrinfo(a, ps, hints, &one))
                for (*tail = one; *tail; tail = &(*tail)->ai_next)
                    ;
        }
    }
    return res;
}

/* Starts a connection to the first address from *at that takes one: the socket, or -1. *at moves past it. */
static int start(struct addrinfo **at)
{
    struct addrinfo *ai;
    int fd, one = 1;
    while ((ai = *at) != NULL) {
        *at = ai->ai_next;
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (!connect(fd, ai->ai_addr, ai->ai_addrlen) || errno == EINPROGRESS)
            return fd;
        close(fd);
    }
    return -1;
}

static int pending(struct env *e, long sock)
{
    int i;
    for (i = 0; i < PENDING; i++)
        if (e->pend[i].res && e->pend[i].fd == sock)
            return i;
    return -1;
}

static void forget(struct env *e, long sock)
{
    int i = pending(e, sock);
    if (i >= 0) {
        freeaddrinfo(e->pend[i].res);
        e->pend[i].res = NULL;
    }
}

static long p_connect(void *env, const char *host, unsigned port, int *error)
{
    struct env *e = env;
    struct addrinfo hints, *res, *next;
    char ps[8];
    int fd, i;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", port);
    if (!(res = resolve(host, ps, &hints))) {
        *error = WFN_E_DNS;
        return -1;
    }
    next = res;
    fd = start(&next);
    /* the rest, for when this one fails on the way (p_connected) */
    if (fd >= 0 && next) {
        for (i = 0; i < PENDING && e->pend[i].res; i++)
            ;
        if (i < PENDING) {
            e->pend[i].fd = fd, e->pend[i].res = res, e->pend[i].next = next;
            res = NULL;
        }
    }
    if (res)
        freeaddrinfo(res);
    if (fd < 0)
        *error = WFN_E_CONNECT;
    return fd;
}

static int p_connected(void *env, long sock)
{
    struct env *e = env;
    struct pollfd p;
    int err = 0, i, fd;
    socklen_t len = sizeof err;
    p.fd = (int)sock, p.events = POLLOUT, p.revents = 0;
    if (poll(&p, 1, 0) <= 0)
        return WFN_AGAIN_W;
    if (getsockopt((int)sock, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
        /* the next address, on the same socket number */
        if ((i = pending(e, sock)) >= 0)
            while ((fd = start(&e->pend[i].next)) >= 0) {
                if (dup3(fd, (int)sock, O_CLOEXEC) >= 0) {
                    close(fd);
                    return WFN_AGAIN_W;
                }
                close(fd);
            }
        forget(e, sock);
        return WFN_E_CONNECT;
    }
    forget(e, sock);
    return 0;
}

static long p_send(void *env, long sock, const void *p, unsigned long n)
{
    ssize_t r = send((int)sock, p, n, MSG_NOSIGNAL);
    (void)env;
    if (r >= 0)
        return (long)r;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? WFN_AGAIN_W : WFN_FAIL;
}

static long p_recv(void *env, long sock, void *p, unsigned long n)
{
    ssize_t r = recv((int)sock, p, n, 0);
    (void)env;
    if (r >= 0)
        return (long)r;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? WFN_AGAIN : WFN_FAIL;
}

static void p_close(void *env, long sock)
{
    forget(env, sock);
    close((int)sock);
}

static int p_wait(void *env, const long *socks, const unsigned char *want, unsigned char *ready, int n, unsigned long ms)
{
    struct pollfd p[128];
    int i, k = n < 128 ? n : 128;
    (void)env;
    for (i = 0; i < k; i++) {
        p[i].fd = (int)socks[i];
        p[i].events = (short)((want[i] & WFN_WANT_READ ? POLLIN : 0) | (want[i] & WFN_WANT_WRITE ? POLLOUT : 0));
        p[i].revents = 0;
    }
    poll(p, (nfds_t)k, (int)ms);
    for (i = 0; i < k; i++)
        ready[i] = (unsigned char)((p[i].revents & (POLLIN | POLLHUP | POLLERR) ? WFN_WANT_READ : 0)
                                   | (p[i].revents & POLLOUT ? WFN_WANT_WRITE : 0));
    return 0;
}

static void *p_tls_new(void *env, long sock, const char *host, const char *alpn)
{
    struct env *e = env;
    struct tls *t = calloc(1, sizeof *t);
    unsigned char wire[64];
    unsigned n = 0;
    const char *p = alpn;
    struct in6_addr a6;
    struct in_addr a4;
    if (!t || !(t->ssl = SSL_new(e->ctx))) {
        free(t);
        return NULL;
    }
    snprintf(t->host, sizeof t->host, "%s", host);
    SSL_set_fd(t->ssl, (int)sock);
    if (inet_pton(AF_INET, host, &a4) != 1 && inet_pton(AF_INET6, host, &a6) != 1) {
        SSL_set_tlsext_host_name(t->ssl, host);
        SSL_set1_host(t->ssl, host);
    } else
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host);
    while (*p && n < sizeof wire - 16) {
        const char *c = strchr(p, ',');
        unsigned l = c ? (unsigned)(c - p) : (unsigned)strlen(p);
        wire[n++] = (unsigned char)l;
        memcpy(wire + n, p, l);
        n += l;
        p += l + (c ? 1 : 0);
    }
    SSL_set_alpn_protos(t->ssl, wire, n);
    SSL_set_connect_state(t->ssl);
    return t;
}

static long ssl_result(SSL *s, int r)
{
    int e = SSL_get_error(s, r);
    if (e == SSL_ERROR_WANT_READ)
        return WFN_AGAIN;
    if (e == SSL_ERROR_WANT_WRITE)
        return WFN_AGAIN_W;
    if (e == SSL_ERROR_ZERO_RETURN)
        return 0;
    return WFN_FAIL;
}

static int p_tls_handshake(void *env, void *tls)
{
    struct tls *t = tls;
    int r = SSL_do_handshake(t->ssl);
    (void)env;
    if (r == 1)
        return 0;
    r = (int)ssl_result(t->ssl, r);
    return r ? r : WFN_FAIL;
}

static long p_tls_read(void *env, void *tls, void *p, unsigned long n)
{
    struct tls *t = tls;
    int r = SSL_read(t->ssl, p, (int)n);
    (void)env;
    if (r > 0)
        return r;
    if (SSL_get_error(t->ssl, r) == SSL_ERROR_SYSCALL && !ERR_peek_error())
        return 0;                                   /* closed without close_notify: the end */
    return ssl_result(t->ssl, r);
}

static long p_tls_write(void *env, void *tls, const void *p, unsigned long n)
{
    struct tls *t = tls;
    int r = SSL_write(t->ssl, p, (int)n);
    (void)env;
    if (r > 0)
        return r;
    r = (int)ssl_result(t->ssl, r);
    return r == WFN_AGAIN ? WFN_AGAIN_W : r ? r : WFN_FAIL;
}

static long p_tls_pending(void *env, void *tls)
{
    (void)env;
    return SSL_pending(((struct tls *)tls)->ssl);
}

static unsigned long p_tls_info(void *env, void *tls, char *out, unsigned long room, int *verified)
{
    struct tls *t = tls;
    X509 *cert = SSL_get1_peer_certificate(t->ssl);
    long v = SSL_get_verify_result(t->ssl);
    int n = 0;
    (void)env;
    *verified = 0;
    if (SSL_is_init_finished(t->ssl)) {
        *verified = v == X509_V_OK;
        n = snprintf(out, room, "tls=%s\ncipher=%s\nverified=%s\n", SSL_get_version(t->ssl),
                     SSL_get_cipher_name(t->ssl), *verified ? "yes" : "no");
    } else if (v != X509_V_OK)
        n = snprintf(out, room, "error=%s\n", X509_verify_cert_error_string(v));
    else {
        unsigned long e = ERR_peek_last_error();
        n = snprintf(out, room, "error=%s\n", e ? ERR_reason_error_string(e) : "the TLS handshake failed");
    }
    if (cert && n >= 0 && (unsigned long)n < room) {
        char s[256];
        X509_NAME_oneline(X509_get_subject_name(cert), s, sizeof s);
        n += snprintf(out + n, room - (unsigned long)n, "subject=%s\n", s);
    }
    X509_free(cert);
    return n < 0 ? 0 : (unsigned long)n < room ? (unsigned long)n : room - 1;
}

static const char *p_tls_alpn(void *env, void *tls)
{
    static __thread char name[32];
    const unsigned char *p;
    unsigned n;
    (void)env;
    SSL_get0_alpn_selected(((struct tls *)tls)->ssl, &p, &n);
    if (n >= sizeof name)
        n = 0;
    memcpy(name, p, n);
    name[n] = 0;
    return name;
}

static void p_tls_free(void *env, void *tls)
{
    struct tls *t = tls;
    (void)env;
    SSL_free(t->ssl);
    free(t);
}

static void p_random(void *env, void *p, unsigned long n)
{
    (void)env;
    if (getrandom(p, n, 0) != (ssize_t)n)
        memset(p, 0x5a, n);
}

static long p_getenv(void *env, const char *name, char *out, unsigned long room)
{
    const char *v = getenv(name);
    (void)env;
    if (!v)
        return -1;
    snprintf(out, room, "%s", v);
    return (long)strlen(out);
}

/* zlib for gzip and deflate, as the datatypes will do it. */
static void *p_dec_new(void *env, const char *coding)
{
    z_stream *z;
    int gz = !strcasecmp(coding, "gzip") || !strcasecmp(coding, "x-gzip");
    if (!((struct env *)env)->decoders || (!gz && strcasecmp(coding, "deflate")))
        return NULL;
    if (!(z = calloc(1, sizeof *z)) || inflateInit2(z, gz ? 31 : 15) != Z_OK) {
        free(z);
        return NULL;
    }
    return z;
}

static long p_dec_run(void *env, void *dec, const unsigned char *in, unsigned long length, unsigned long *used,
                      unsigned char *out, unsigned long room, int last)
{
    z_stream *z = dec;
    int r;
    (void)env;
    (void)last;
    z->next_in = (unsigned char *)in;
    z->avail_in = (uInt)length;
    z->next_out = out;
    z->avail_out = (uInt)room;
    r = inflate(z, Z_NO_FLUSH);
    *used = length - z->avail_in;
    if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
        return -1;
    return (long)(room - z->avail_out);
}

static void p_dec_free(void *env, void *dec)
{
    (void)env;
    inflateEnd(dec);
    free(dec);
}

static const char *p_dec_accept(void *env)
{
#ifdef WFNP_DATATYPES
    return ((struct env *)env)->decoders ? wfn_dec_accept(env, &((struct env *)env)->dt_seen) : NULL;
#else
    return ((struct env *)env)->decoders ? "gzip, deflate" : NULL;
#endif
}

static const char *p_dec_name(void *env, void *dec)
{
    (void)env;
    (void)dec;
    return "zlib";
}

/* For the tests (ctypes): a struct wfn with this machine under it. decoders: zlib (or the datatypes'
 * glue) for WFN_F_DECODE. */
struct wfnp {
    struct wfn *w;
    struct env *e;
};

struct wfnp *wfnp_new(unsigned long window, int decoders)
{
    struct wfn_io io;
    struct wfnp *p = calloc(1, sizeof *p);
    struct env *e = calloc(1, sizeof *e);
    if (!p || !e || !(e->ctx = SSL_CTX_new(TLS_client_method()))) {
        free(p);
        free(e);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(e->ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(e->ctx);
    SSL_CTX_set_verify(e->ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_mode(e->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    e->decoders = decoders;
    memset(&io, 0, sizeof io);
    io.env = e;
    io.alloc = p_alloc;
    io.release = p_release;
    io.now = p_now;
    io.connect = p_connect;
    io.connected = p_connected;
    io.send = p_send;
    io.recv = p_recv;
    io.close = p_close;
    io.wait = p_wait;
    io.tls_new = p_tls_new;
    io.tls_handshake = p_tls_handshake;
    io.tls_read = p_tls_read;
    io.tls_write = p_tls_write;
    io.tls_pending = p_tls_pending;
    io.tls_info = p_tls_info;
    io.tls_alpn = p_tls_alpn;
    io.tls_free = p_tls_free;
    io.random = p_random;
    io.getenv = p_getenv;
    io.dec_new = p_dec_new;
    io.dec_run = p_dec_run;
    io.dec_free = p_dec_free;
    io.dec_name = p_dec_name;
#ifdef WFNP_DATATYPES
    if (decoders) {
        io.dec_new = wfn_dec_new;
        io.dec_run = wfn_dec_run;
        io.dec_free = wfn_dec_free;
        io.dec_name = wfn_dec_name;
    }
#endif
    io.dec_accept = p_dec_accept;
    p->e = e;
    if (!(p->w = wfn_new(&io, window))) {
        SSL_CTX_free(e->ctx);
        free(e);
        free(p);
        return NULL;
    }
    return p;
}

long wfnp_call(struct wfnp *p, unsigned op, unsigned long arg, const unsigned long extra[4], struct wfn_buf buf[4],
               unsigned long *result, unsigned long *aux)
{
    return wfn_call(p->w, op, arg, extra, buf, result, aux);
}

void wfnp_free(struct wfnp *p)
{
    wfn_free(p->w);
    SSL_CTX_free(p->e->ctx);
    free(p->e);
    free(p);
}

void wfnp_pump(struct wfnp *p, unsigned long ms)
{
    wfn_pump(p->w, ms);
}
