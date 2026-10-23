/*
 * wfnet: openweb.fetch on the machine itself (include/openweb/wfnet.h,
 * docs/OPENWEB_FETCH.md "The native transport").
 *
 * A fetch is a struct wfn_fetch; a connection a struct wfn_conn. Nothing
 * blocks: sockets are non-blocking, and every call runs progress(), which
 * moves each connection on as far as it can (connect, CONNECT through a
 * proxy, TLS, then HTTP/1.1, HTTP/2 or WebSocket), then waits on all the
 * sockets for what is left of the call's wait. So a task's fetches all move
 * whichever of them it is asking about.
 *
 * HTTP/1.1 here; HTTP/2 in wfnet_h2.c, HPACK in wfnet_hpack.c, the hashes
 * and auth answers in wfnet_auth.c.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "openweb/wfnet.h"
#include "wfnet_int.h"

#define USER_AGENT     "OpenWeb-Fetch/2"
#define TIMEOUT_DEF    30UL
#define TIMEOUT_MAX    300UL
#define MAX_WAIT       2000UL
#define KEEP_PER_HOST  6
#define KEEP_MAX       32
#define KEEP_IDLE      30000UL
#define WS_RECORD      5            /* a WebSocket record in the ring: flags, then the length */
#define MAX_RETRIES    2

/* ---- small helpers ----------------------------------------------------- */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int wfn_ieq(const char *a, unsigned long n, const char *b)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        if (!b[i] || lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
    return !b[n];
}

/* Copies n bytes of s, NUL ended, cut to fit: 0, or -1 when cut. */
static int cpy(char *d, unsigned long room, const char *s, unsigned long n)
{
    int cut = n >= room;
    if (cut)
        n = room - 1;
    memcpy(d, s, n);
    d[n] = 0;
    return cut ? -1 : 0;
}

/* v in decimal (base 10) or hex (base 16), NUL ended: its length. */
static unsigned num(char *out, unsigned long v, unsigned base)
{
    char t[24];
    unsigned n = 0, i;
    do
        t[n++] = "0123456789abcdef"[v % base];
    while (v /= base);
    for (i = 0; i < n; i++)
        out[i] = t[n - 1 - i];
    out[n] = 0;
    return n;
}

/* A number at p (n bytes): -1 when none or too big. */
static long parse_num(const char *p, unsigned long n, unsigned base, unsigned long *used)
{
    unsigned long v = 0, i = 0;
    for (; i < n; i++) {
        int c = lower((unsigned char)p[i]), d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            break;
        if (v > 0x7ffffffUL)
            return -1;
        v = v * base + (unsigned long)d;
    }
    if (used)
        *used = i;
    return i ? (long)v : -1;
}

static int tchar(int c)
{
    return c > ' ' && c < 127 && !strchr("\"(),/:;<=>?@[\\]{}", c);
}

static void *zalloc(struct wfn *w, unsigned long n)
{
    return w->io.alloc(w->io.env, n ? n : 1);
}

static void zfree(struct wfn *w, void *p)
{
    if (p)
        w->io.release(w->io.env, p);
}

static unsigned long now(struct wfn *w)
{
    return w->io.now(w->io.env);
}

static int passed(unsigned long now_ms, unsigned long when)
{
    return (long)(now_ms - when) >= 0;
}

/* Accept-Encoding with WFN_F_DECODE: what the decoders undo, or identity. */
const char *wfn_accept(struct wfn *w)
{
    const char *a = w->io.dec_accept ? w->io.dec_accept(w->io.env) : NULL;
    return a && *a ? a : "identity";
}

void wfn_info(struct wfn_fetch *f, const char *key, const char *value)
{
    unsigned long kl = strlen(key), vl = strlen(value);
    if (f->info_len + kl + vl + 2 >= sizeof f->info)
        return;
    memcpy(f->info + f->info_len, key, kl);
    f->info[f->info_len + kl] = '=';
    memcpy(f->info + f->info_len + kl + 1, value, vl);
    f->info_len += kl + vl + 2;
    f->info[f->info_len - 1] = '\n';
}

/* A header's value in "Name: value" lines (LF or CRLF), from *from on (or
 * the start): its value and length, and *from after its line; NULL when
 * there is no more. */
const char *wfn_hdr(const char *text, unsigned long len, const char *name, unsigned long *vl, const char **from)
{
    const char *p = from && *from ? *from : text, *end = text + len;
    unsigned long n = strlen(name);
    while (p < end) {
        const char *nl = p, *v, *e;
        while (nl < end && *nl != '\n')
            nl++;
        if ((unsigned long)(nl - p) > n && p[n] == ':' && wfn_ieq(p, n, name)) {
            v = p + n + 1;
            e = nl > p && nl[-1] == '\r' ? nl - 1 : nl;
            while (v < e && (*v == ' ' || *v == '\t'))
                v++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
                e--;
            *vl = (unsigned long)(e - v);
            if (from)
                *from = nl < end ? nl + 1 : end;
            return v;
        }
        p = nl + 1;
    }
    return NULL;
}

/* Whether a comma list (a header's value) holds a token, in any case. */
static int has_token(const char *v, unsigned long n, const char *token)
{
    unsigned long i = 0;
    while (i < n) {
        unsigned long s, e;
        while (i < n && (v[i] == ',' || v[i] == ' ' || v[i] == '\t'))
            i++;
        s = i;
        while (i < n && v[i] != ',' && v[i] != ';')
            i++;
        e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t'))
            e--;
        if (e > s && wfn_ieq(v + s, e - s, token))
            return 1;
        while (i < n && v[i] != ',')
            i++;
    }
    return 0;
}

/* ---- the ring a fetch's body waits in ---------------------------------- */

static unsigned long ring_put(struct wfn_ring *r, const unsigned char *p, unsigned long n)
{
    unsigned long done = 0;
    while (n && r->len < r->size) {
        unsigned long end = (r->at + r->len) % r->size;
        unsigned long run = (end >= r->at ? r->size - end : r->at - end);
        if (run > n)
            run = n;
        if (run > r->size - r->len)
            run = r->size - r->len;
        memcpy(r->p + end, p, run);
        r->len += run, p += run, n -= run, done += run;
    }
    return done;
}

static unsigned long ring_get(struct wfn_ring *r, unsigned char *out, unsigned long n)
{
    unsigned long done = 0;
    while (n && r->len) {
        unsigned long run = r->size - r->at;
        if (run > r->len)
            run = r->len;
        if (run > n)
            run = n;
        if (out)
            memcpy(out + done, r->p + r->at, run);
        r->at = (r->at + run) % r->size;
        r->len -= run, n -= run, done += run;
    }
    if (!r->len)
        r->at = 0;
    return done;
}

/* The free run at the ring's end, for the decoder to write into. */
static unsigned char *ring_space(struct wfn_ring *r, unsigned long *n)
{
    unsigned long end = (r->at + r->len) % r->size;
    *n = r->len == r->size ? 0 : end >= r->at ? r->size - end : r->at - end;
    return r->p + end;
}

static int ring_new(struct wfn *w, struct wfn_ring *r)
{
    r->size = w->window;
    r->at = r->len = 0;
    return (r->p = zalloc(w, r->size)) != NULL;
}

/* ---- the body to send ----------------------------------------------------- */

static int up_add(struct wfn *w, struct wfn_fetch *f, const unsigned char *p, unsigned long n)
{
    struct wfn_piece *piece;
    if (!n)
        return 0;
    if (!(piece = zalloc(w, sizeof *piece + n)))
        return -1;
    memcpy(piece->data, p, n);
    piece->length = n;
    if (f->up_tail)
        f->up_tail->next = piece;
    else
        f->up_head = piece;
    f->up_tail = piece;
    if (!f->up_cur)
        f->up_cur = piece, f->up_cur_at = 0;
    f->up_queued += n;
    f->up_given += n;
    return 0;
}

/* The next body bytes to send; consumed ones are freed unless the body is kept for sending again. */
static void up_trim(struct wfn *w, struct wfn_fetch *f);

unsigned long wfn_up_take(struct wfn *w, struct wfn_fetch *f, unsigned char *out, unsigned long room)
{
    unsigned long done = 0;
    while (room && f->up_cur) {
        struct wfn_piece *p = f->up_cur;
        unsigned long run = p->length - f->up_cur_at;
        if (run > room)
            run = room;
        memcpy(out + done, p->data + f->up_cur_at, run);
        done += run, room -= run, f->up_cur_at += run;
        if (f->up_cur_at == p->length) {
            f->up_cur = p->next;
            f->up_cur_at = 0;
        }
    }
    f->up_queued -= done;
    f->up_sent += done;
    up_trim(w, f);
    return done;
}

static void up_trim(struct wfn *w, struct wfn_fetch *f)
{
    if (f->keep_body)
        return;
    while (f->up_head && f->up_head != f->up_cur) {
        struct wfn_piece *p = f->up_head;
        f->up_head = p->next;
        zfree(w, p);
    }
    if (!f->up_head)
        f->up_tail = NULL;
}

int wfn_up_done(const struct wfn_fetch *f)
{
    if (!(f->flags & WFN_F_BODY))
        return 1;
    if (f->up_cur)
        return 0;
    return f->body_length == WFN_LENGTH_UNKNOWN ? f->up_last : f->up_given >= f->body_length;
}

int wfn_resendable(const struct wfn_fetch *f)
{
    return !(f->flags & WFN_F_BODY) || f->keep_body;
}

/* Whether a fetch may go again after the connection failed with no answer: never sent, or a method safe to repeat. */
static int retry_safe(const struct wfn_fetch *f)
{
    const char *m = f->method;
    if (!wfn_resendable(f))
        return 0;
    return !f->sent_request || !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "OPTIONS") || !strcmp(m, "TRACE")
        || !strcmp(m, "PUT") || !strcmp(m, "DELETE");
}

/* ---- connections ------------------------------------------------------- */

int wfn_out(struct wfn *w, struct wfn_conn *c, const void *p, unsigned long n)
{
    if (c->out_at && c->out_at == c->out_len)
        c->out_at = c->out_len = 0;
    if (c->out_len + n > c->out_room) {
        unsigned long room = c->out_room ? c->out_room : 4096, keep = c->out_len - c->out_at;
        unsigned char *b;
        while (room < keep + n)
            room *= 2;
        if (!(b = zalloc(w, room)))
            return -1;
        if (keep)
            memcpy(b, c->out + c->out_at, keep);
        zfree(w, c->out);
        c->out = b, c->out_room = room, c->out_at = 0, c->out_len = keep;
    }
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
    return 0;
}

static int outs(struct wfn *w, struct wfn_conn *c, const char *s)
{
    return wfn_out(w, c, s, strlen(s));
}

static unsigned long out_waiting(const struct wfn_conn *c)
{
    return c->out_len - c->out_at;
}

/* Sends what it can: 0, or -1 when the connection failed. */
int wfn_flush(struct wfn *w, struct wfn_conn *c)
{
    c->blocked_w = 0;
    while (c->out_at < c->out_len) {
        long n = c->tls ? w->io.tls_write(w->io.env, c->tls, c->out + c->out_at, c->out_len - c->out_at)
                        : w->io.send(w->io.env, c->sock, c->out + c->out_at, c->out_len - c->out_at);
        if (n == WFN_AGAIN || n == WFN_AGAIN_W) {
            c->blocked_w = 1;
            return 0;
        }
        if (n <= 0)
            return -1;
        c->out_at += (unsigned long)n;
    }
    c->out_at = c->out_len = 0;
    return 0;
}

/* Reads into the connection's buffer: bytes read, 0 at the end, WFN_AGAIN, or WFN_FAIL (or when full, WFN_AGAIN). */
long wfn_conn_read(struct wfn *w, struct wfn_conn *c)
{
    long n;
    if (c->in_at) {
        memmove(c->in, c->in + c->in_at, c->in_len - c->in_at);
        c->in_len -= c->in_at;
        c->in_at = 0;
    }
    if (c->in_len == sizeof c->in)
        return WFN_AGAIN;
    n = c->tls ? w->io.tls_read(w->io.env, c->tls, c->in + c->in_len, sizeof c->in - c->in_len)
               : w->io.recv(w->io.env, c->sock, c->in + c->in_len, sizeof c->in - c->in_len);
    if (n > 0)
        c->in_len += (unsigned long)n;
    else if (n == WFN_AGAIN_W)
        n = WFN_AGAIN;
    else if (n < 0 && n != WFN_AGAIN)
        n = WFN_FAIL;
    return n;
}

static void conn_kill(struct wfn *w, struct wfn_conn *c)
{
    (void)w;
    c->state = C_DEAD;
    c->owner = NULL;
}

static void conn_free(struct wfn *w, struct wfn_conn *c)
{
    struct wfn_conn **pp = &w->conns;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;
    wfn_h2_free(w, c);
    if (c->tls)
        w->io.tls_free(w->io.env, c->tls);
    if (c->sock >= 0)
        w->io.close(w->io.env, c->sock);
    zfree(w, c->out);
    zfree(w, c);
    w->nconn--;
}

/* ---- fetches: failing, ending, starting again ----------------------------- */

static void reset_answer(struct wfn *w, struct wfn_fetch *f)
{
    zfree(w, f->htext);
    f->htext = NULL;
    f->hlen = f->hbuf_len = 0;
    f->status = f->hflags = f->version = f->length = 0;
    f->framing = FR_NONE;
    f->chunk_state = 0;
    f->left = 0;
    f->got_bytes = 0;
    f->discard = 0;
    if (f->dec) {
        w->io.dec_free(w->io.env, f->dec);
        f->dec = NULL;
    }
    f->raw.len = f->raw.at = 0;
    f->no_decoder = 0;
}

void wfn_fail(struct wfn *w, struct wfn_fetch *f, unsigned long error, const char *why)
{
    struct wfn_conn *c = f->conn;
    if (f->body_done && (f->error || f->state == F_DONE))
        return;
    f->error = error;
    if (why)
        wfn_info(f, "error", why);
    if (c) {
        if (c->h2)
            wfn_h2_detach(w, c, f, 1);
        else if (c->owner == f)
            conn_kill(w, c);
    }
    f->conn = NULL;
    f->body_done = 1;
    f->state = F_DONE;
}

/* Back to waiting for a connection, to send the request again. */
void wfn_requeue(struct wfn *w, struct wfn_fetch *f)
{
    reset_answer(w, f);
    f->head_ready = 0;
    f->conn = NULL;
    f->sid = 0;
    f->end_sent = f->end_recv = 0;
    f->sent_request = 0;
    f->reused = 0;
    f->connect_phase = 0;
    f->up_cur = f->up_head;
    f->up_cur_at = 0;
    f->up_queued = f->up_given;
    f->up_sent = 0;
    f->state = F_WAIT;
    f->deadline = now(w) + f->timeout_ms;
}

/* The connection failed: each fetch on it fails, or goes again when it can. */
void wfn_conn_fail(struct wfn *w, struct wfn_conn *c, unsigned long error, const char *why)
{
    struct wfn_fetch *f;
    for (f = w->fetches; f; f = f->next) {
        if (f->conn != c)
            continue;
        if (c->h2)
            wfn_h2_detach(w, c, f, 0);
        f->conn = NULL;
        if (f->state == F_WS) {
            f->body_done = 1;
            if (!f->close_code)
                f->error = error;
            continue;
        }
        if (!f->got_bytes && (c->reused || c->h2) && f->retries < MAX_RETRIES && retry_safe(f)
            && f->state != F_DONE) {
            f->retries++;
            wfn_requeue(w, f);
        } else
            wfn_fail(w, f, error, why);
    }
    conn_kill(w, c);
}

/* Body bytes for the caller: how many were taken (all, unless the ring or the decoder is full). */
unsigned long wfn_deliver(struct wfn *w, struct wfn_fetch *f, const unsigned char *p, unsigned long n)
{
    unsigned long used = 0;
    if (n)
        f->deadline = now(w) + f->timeout_ms;
    if (f->discard)
        return n;
    if (!f->dec)
        return ring_put(&f->ring, p, n);
    while (used < n) {
        unsigned long room, u = 0;
        unsigned char *space = ring_space(&f->ring, &room);
        long got;
        if (!room)
            break;
        got = w->io.dec_run(w->io.env, f->dec, p + used, n - used, &u, space, room, 0);
        if (got < 0) {
            wfn_fail(w, f, WFN_E_DECODE, "the coded body was bad");
            return n;
        }
        f->ring.len += (unsigned long)got;
        used += u;
        if (!got && !u)
            break;
    }
    return used;
}

/* Keeps body bytes the ring could not take (HTTP/2 DATA must be taken whole). */
void wfn_stash(struct wfn *w, struct wfn_fetch *f, const unsigned char *p, unsigned long n)
{
    if (!n)
        return;
    if (!f->raw.p && !ring_new(w, &f->raw)) {
        wfn_fail(w, f, WFN_E_HOST, "no memory");
        return;
    }
    if (ring_put(&f->raw, p, n) < n)
        wfn_fail(w, f, WFN_E_PROTOCOL, "the server sent more than the window");
}

/* Moves stashed bytes on as the ring empties. */
static int drain(struct wfn *w, struct wfn_fetch *f)
{
    int moved = 0;
    while (f->raw.len) {
        unsigned long run = f->raw.size - f->raw.at, took;
        if (run > f->raw.len)
            run = f->raw.len;
        if (!(took = wfn_deliver(w, f, f->raw.p + f->raw.at, run)))
            break;
        ring_get(&f->raw, NULL, took);
        moved = 1;
        if (f->dec && f->conn && f->conn->h2)
            wfn_h2_consumed(w, f->conn, f, took);
    }
    return moved;
}

/* Flushes the decoder at the body's end: 1 while it still has output for a full ring. */
static int dec_finish(struct wfn *w, struct wfn_fetch *f)
{
    for (;;) {
        unsigned long room, u = 0;
        unsigned char *space = ring_space(&f->ring, &room);
        long got;
        if (!room)
            return 1;
        got = w->io.dec_run(w->io.env, f->dec, (const unsigned char *)"", 0, &u, space, room, 1);
        if (got < 0) {
            wfn_fail(w, f, WFN_E_DECODE, "the coded body was bad");
            return 0;
        }
        if (!got)
            return 0;
        f->ring.len += (unsigned long)got;
    }
}

static void start_over(struct wfn *w, struct wfn_fetch *f);

/* The answer's body has all come (HTTP/1.1's connection already let go). */
void wfn_body_end(struct wfn *w, struct wfn_fetch *f)
{
    if (f->discard) {
        start_over(w, f);
        return;
    }
    f->end_recv = 1;
    if (f->raw.len || (f->dec && dec_finish(w, f)))
        return;                                 /* progress() ends it when the rest is in */
    f->body_done = 1;
    f->state = F_DONE;
}

/* ---- the head ------------------------------------------------------------- */

static unsigned long random_hex(struct wfn *w, char *out, unsigned n)
{
    unsigned char r[16];
    unsigned i;
    w->io.random(w->io.env, r, n);
    for (i = 0; i < n; i++)
        out[2 * i] = "0123456789abcdef"[r[i] >> 4], out[2 * i + 1] = "0123456789abcdef"[r[i] & 15];
    out[2 * n] = 0;
    return 2 * n;
}

static void authority(const struct wfn_fetch *f, char *out, int always_port);

/* An answer to 401 or 407: the Authorization value, or NULL. */
static char *answer(struct wfn *w, struct wfn_fetch *f, const char *lines, unsigned long len, int proxy)
{
    char all[2048], cnonce[33], uri[WFN_HOST_ROOM + 16], *out, *full = NULL;
    const char *from = NULL, *v, *target = f->target;
    unsigned long vl, at = 0, n;
    while ((v = wfn_hdr(lines, len, proxy ? "Proxy-Authenticate" : "WWW-Authenticate", &vl, &from)) != NULL) {
        if (at + vl + 2 >= sizeof all)
            break;
        if (at)
            all[at++] = ',';
        memcpy(all + at, v, vl);
        at += vl;
    }
    if (!at || !(out = zalloc(w, 1024)))
        return NULL;
    random_hex(w, cnonce, 16);
    if (f->connect_phase) {
        n = (unsigned long)strlen(f->host);
        cpy(uri, sizeof uri - 7, f->host, n);
        uri[n] = ':';
        num(uri + n + 1, f->port, 10);
        target = uri;
    } else if (proxy && f->forward) {
        /* the proxy sees the absolute form, as the request line has it */
        authority(f, uri, 0);
        n = (unsigned long)strlen(uri);
        if (!(full = zalloc(w, 8 + n + strlen(f->target)))) {
            zfree(w, out);
            return NULL;
        }
        memcpy(full, "http://", 7);
        memcpy(full + 7, uri, n);
        strcpy(full + 7 + n, f->target);
        target = full;
    }
    if (!wfn_auth_answer(all, at, proxy ? f->puser : f->user, proxy ? f->ppass : f->pass,
                         f->connect_phase ? "CONNECT" : f->method, target, 1, cnonce, out, 1024)) {
        zfree(w, full);
        zfree(w, out);
        return NULL;
    }
    zfree(w, full);
    return out;
}

/* Whether a header is the connection's own, never passed on. */
static int hop_header(const char *name, unsigned long nl)
{
    return wfn_ieq(name, nl, "transfer-encoding") || wfn_ieq(name, nl, "connection") || wfn_ieq(name, nl, "keep-alive")
        || wfn_ieq(name, nl, "proxy-connection") || wfn_ieq(name, nl, "upgrade") || wfn_ieq(name, nl, "te")
        || wfn_ieq(name, nl, "host");
}

int wfn_set_head(struct wfn *w, struct wfn_fetch *f, unsigned long status, unsigned long version, const char *sline,
                 unsigned long sl, const char *lines, unsigned long len)
{
    const char *p, *end = lines + len, *v;
    unsigned long vl, at;
    int drop_coding = 0;
    char coding[32];
    /* 401 or 407 with credentials: answer the challenge and ask again. */
    if ((status == 401 && f->user[0] && !f->auth_tried && !f->connect_phase)
        || (status == 407 && f->puser[0] && !f->pauth_tried)) {
        int proxy = status == 407;
        char *a = wfn_resendable(f) ? answer(w, f, lines, len, proxy) : NULL;
        if (a) {
            if (proxy) {
                zfree(w, f->pauth);
                f->pauth = a;
                f->pauth_tried = 1;
            } else {
                zfree(w, f->auth);
                f->auth = a;
                f->auth_tried = 1;
            }
            f->discard = 1;
            return 1;
        }
    }
    /* Content-Encoding: through the decoder hook when there is one. */
    if ((f->flags & WFN_F_DECODE) && (v = wfn_hdr(lines, len, "content-encoding", &vl, NULL)) != NULL && vl
        && !wfn_ieq(v, vl, "identity")) {
        if (!cpy(coding, sizeof coding, v, vl) && w->io.dec_new && (f->dec = w->io.dec_new(w->io.env, coding)) != NULL)
            drop_coding = 1;
        else
            f->no_decoder = 1;
    }
    if (!(f->htext = zalloc(w, sl + len + 2)))
        return -1;
    memcpy(f->htext, sline, sl);
    at = sl;
    f->htext[at++] = '\n';
    for (p = lines; p < end;) {
        const char *nl = p, *e, *colon;
        while (nl < end && *nl != '\n')
            nl++;
        e = nl > p && nl[-1] == '\r' ? nl - 1 : nl;
        for (colon = p; colon < e && *colon != ':'; colon++)
            ;
        if (e > p && !(colon < e && (wfn_ieq(p, (unsigned long)(colon - p), "transfer-encoding")
                                     || (drop_coding && (wfn_ieq(p, (unsigned long)(colon - p), "content-encoding")
                                                         || wfn_ieq(p, (unsigned long)(colon - p), "content-length")))))) {
            memcpy(f->htext + at, p, (unsigned long)(e - p));
            at += (unsigned long)(e - p);
            f->htext[at++] = '\n';
        }
        p = nl + 1;
    }
    f->hlen = at;
    f->status = status;
    f->version = version;
    if (drop_coding) {
        f->hflags |= WFN_H_DECODED;
        f->hflags &= ~WFN_H_LENGTH;
    }
    if (f->use_tls)
        f->hflags |= WFN_H_TLS;
    if (f->conn) {
        struct wfn_conn *c = f->conn;
        if (c->verified)
            f->hflags |= WFN_H_VERIFIED;
        if (c->tls_info_len && f->info_len + c->tls_info_len < sizeof f->info) {
            memcpy(f->info + f->info_len, c->tls_info, c->tls_info_len);
            f->info_len += c->tls_info_len;
        }
        if (c->h2)
            wfn_info(f, "alpn", "h2");
    }
    if (f->phost[0]) {
        char hp[WFN_HOST_ROOM + 8];
        unsigned long n = strlen(f->phost);
        memcpy(hp, f->phost, n);
        hp[n] = ':';
        num(hp + n + 1, f->pport, 10);
        wfn_info(f, "proxy", hp);
    }
    if (f->auth_tried || f->pauth_tried)
        wfn_info(f, "auth", f->auth_tried && f->pauth_tried ? "both" : f->auth_tried ? "server" : "proxy");
    if (f->no_decoder)
        wfn_info(f, "decode", "datatypes");
    else if (f->dec)
        wfn_info(f, "decode", w->io.dec_name ? w->io.dec_name(w->io.env, f->dec) : "yes");
    if (f->reused)
        wfn_info(f, "reused", "yes");
    f->head_ready = 1;
    f->got_bytes = 1;
    return 0;
}

/* ---- HTTP/1.1 ---------------------------------------------------------------- */

/* "host" or "[v6]", with ":port" unless it is the scheme's own. */
static void authority(const struct wfn_fetch *f, char *out, int always_port)
{
    unsigned long n = 0;
    int v6 = strchr(f->host, ':') != NULL;
    if (v6)
        out[n++] = '[';
    memcpy(out + n, f->host, strlen(f->host));
    n += strlen(f->host);
    if (v6)
        out[n++] = ']';
    if (always_port || f->port != (f->use_tls ? 443U : 80U)) {
        out[n++] = ':';
        n += num(out + n, f->port, 10);
    }
    out[n] = 0;
}

static int write_request(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    char a[WFN_HOST_ROOM + 16], n[24];
    int bad = 0;
    if (f->connect_phase) {
        authority(f, a, 1);
        bad |= outs(w, c, "CONNECT ") | outs(w, c, a) | outs(w, c, " HTTP/1.1\r\nHost: ") | outs(w, c, a)
             | outs(w, c, "\r\nUser-Agent: " USER_AGENT "\r\n");
        if (f->pauth)
            bad |= outs(w, c, "Proxy-Authorization: ") | outs(w, c, f->pauth) | outs(w, c, "\r\n");
        bad |= outs(w, c, "\r\n");
        return bad ? -1 : 0;
    }
    authority(f, a, 0);
    bad |= outs(w, c, f->method) | outs(w, c, " ");
    if (f->forward)
        bad |= outs(w, c, "http://") | outs(w, c, a);
    bad |= outs(w, c, f->target) | outs(w, c, " HTTP/1.1\r\nHost: ") | outs(w, c, a) | outs(w, c, "\r\n");
    bad |= wfn_out(w, c, f->headers, f->headers_len);
    if (!f->has_ua)
        bad |= outs(w, c, "User-Agent: " USER_AGENT "\r\n");
    if (f->ws)
        bad |= outs(w, c, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ") | outs(w, c, f->ws_key)
             | outs(w, c, "\r\nSec-WebSocket-Version: 13\r\n");
    else
        bad |= outs(w, c, "Connection: keep-alive\r\n");
    if (f->flags & WFN_F_DECODE)
        bad |= outs(w, c, "Accept-Encoding: ") | outs(w, c, wfn_accept(w)) | outs(w, c, "\r\n");
    if (f->auth)
        bad |= outs(w, c, "Authorization: ") | outs(w, c, f->auth) | outs(w, c, "\r\n");
    if (f->forward && f->pauth)
        bad |= outs(w, c, "Proxy-Authorization: ") | outs(w, c, f->pauth) | outs(w, c, "\r\n");
    if (f->flags & WFN_F_BODY) {
        if (f->body_length == WFN_LENGTH_UNKNOWN)
            bad |= outs(w, c, "Transfer-Encoding: chunked\r\n");
        else {
            num(n, f->body_length, 10);
            bad |= outs(w, c, "Content-Length: ") | outs(w, c, n) | outs(w, c, "\r\n");
        }
    }
    bad |= outs(w, c, "\r\n");
    return bad ? -1 : 0;
}

/* Moves body bytes into the connection's queue, chunked when the length is not known. */
static int h1_upload(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    unsigned char buf[4096];
    int moved = 0;
    while (!f->end_sent && out_waiting(c) < 16384) {
        unsigned long n = wfn_up_take(w, f, buf, sizeof buf);
        int chunked = f->body_length == WFN_LENGTH_UNKNOWN;
        if (n) {
            char size[16];
            if (chunked) {
                num(size, n, 16);
                if (outs(w, c, size) || outs(w, c, "\r\n"))
                    return -1;
            }
            if (wfn_out(w, c, buf, n) || (chunked && outs(w, c, "\r\n")))
                return -1;
            moved = 1;
            continue;
        }
        if (wfn_up_done(f)) {
            if (chunked && outs(w, c, "0\r\n\r\n"))
                return -1;
            f->end_sent = 1;
            moved = 1;
        }
        break;
    }
    return moved;
}

/* A line in the connection's buffer: its length with the LF, or 0 when it has not all come. */
static unsigned long line(const struct wfn_conn *c)
{
    unsigned long i;
    for (i = c->in_at; i < c->in_len; i++)
        if (c->in[i] == '\n')
            return i - c->in_at + 1;
    return 0;
}

static void ws_opened(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f);

/* The head is in hbuf: 0, 1 when it was a 1xx to skip, -1 when the fetch failed. */
static int h1_head(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    const char *h = f->hbuf, *p, *lines, *v;
    unsigned long n = f->hbuf_len, sl, vl, version, status, used;
    long cl;
    int r;
    for (sl = 0; sl < n && h[sl] != '\n'; sl++)
        ;
    lines = h + sl + 1;
    if (sl && h[sl - 1] == '\r')
        sl--;
    if (sl < 12 || memcmp(h, "HTTP/1.", 7) || (h[7] != '0' && h[7] != '1') || h[8] != ' '
        || parse_num(h + 9, 3, 10, &used) < 100 || used != 3) {
        wfn_fail(w, f, WFN_E_PROTOCOL, "the answer was not HTTP/1.x");
        return -1;
    }
    version = h[7] == '1' ? 11 : 10;
    status = (unsigned long)parse_num(h + 9, 3, 10, NULL);
    n -= (unsigned long)(lines - h);
    if (status < 200 && !(status == 101 && f->ws)) {
        f->hbuf_len = 0;
        return 1;
    }
    /* Framing (RFC 9112 section 6.3), and whether the connection can be kept. */
    f->keep = version == 11;
    if ((v = wfn_hdr(lines, n, "connection", &vl, NULL)) != NULL) {
        if (has_token(v, vl, "close"))
            f->keep = 0;
        else if (has_token(v, vl, "keep-alive"))
            f->keep = 1;
    }
    f->framing = FR_CLOSE;
    if (wfn_ieq(f->method, strlen(f->method), "HEAD") || status == 204 || status == 304 || status == 101
        || (f->connect_phase && status / 100 == 2))
        f->framing = FR_NONE;
    else if ((v = wfn_hdr(lines, n, "transfer-encoding", &vl, NULL)) != NULL && has_token(v, vl, "chunked"))
        f->framing = FR_CHUNKED;
    else if ((v = wfn_hdr(lines, n, "content-length", &vl, NULL)) != NULL) {
        if ((cl = parse_num(v, vl, 10, &used)) < 0 || used != vl) {
            wfn_fail(w, f, WFN_E_PROTOCOL, "a bad Content-Length");
            return -1;
        }
        f->framing = FR_LENGTH;
        f->left = f->length = (unsigned long)cl;
        f->hflags |= WFN_H_LENGTH;
    }
    if (f->framing == FR_CLOSE || f->connect_phase)
        f->keep = 0;
    if (f->framing == FR_NONE) {
        f->hflags |= WFN_H_NOBODY;
        f->length = 0;
    }
    c->idle_ms = KEEP_IDLE;
    if ((v = wfn_hdr(lines, n, "keep-alive", &vl, NULL)) != NULL) {
        for (p = v; p + 8 <= v + vl; p++)
            if (wfn_ieq(p, 8, "timeout=") && (cl = parse_num(p + 8, (unsigned long)(v + vl - p - 8), 10, NULL)) >= 0) {
                if (cl <= 1)
                    f->keep = 0;
                else if ((unsigned long)cl * 1000 - 1000 < c->idle_ms)
                    c->idle_ms = (unsigned long)cl * 1000 - 1000;
                break;
            }
    }
    /* Through a proxy, CONNECT's 2xx: the tunnel is there; TLS (or the request) next. */
    if (f->connect_phase && status / 100 == 2) {
        f->connect_phase = 0;
        f->sent_request = 0;
        f->end_sent = 0;
        f->got_bytes = 0;
        f->hbuf_len = 0;
        f->framing = FR_NONE;
        f->hflags = 0;
        if (c->use_tls) {
            c->state = C_TLS;
            c->tls = w->io.tls_new(w->io.env, c->sock, c->host, c->may_h2 ? "h2,http/1.1" : "http/1.1");
            if (!c->tls) {
                wfn_conn_fail(w, c, WFN_E_TLS, "TLS could not be set up");
                return -1;
            }
            f->state = F_CONNECTING;
        } else {
            c->state = C_READY;
            f->state = F_SEND;
        }
        return 0;
    }
    if (f->ws && status == 101) {
        char want[29];
        wfn_ws_accept(f->ws_key, want);
        v = wfn_hdr(lines, n, "sec-websocket-accept", &vl, NULL);
        if (!v || vl != 28 || memcmp(v, want, 28)) {
            wfn_fail(w, f, WFN_E_PROTOCOL, "the server's Sec-WebSocket-Accept was wrong");
            return -1;
        }
    }
    r = wfn_set_head(w, f, status, version, h, sl, lines, n);
    if (r < 0) {
        wfn_fail(w, f, WFN_E_HOST, "no memory");
        return -1;
    }
    if (f->ws && status == 101 && !r) {
        ws_opened(w, c, f);
        return 0;
    }
    f->state = F_BODY;
    return 0;
}

static void start_over(struct wfn *w, struct wfn_fetch *f)
{
    wfn_requeue(w, f);
}

/* The answer is done: the connection kept, or closed. */
static void h1_release(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    if (f->keep && f->end_sent && c->in_at == c->in_len && !f->ws) {
        c->owner = NULL;
        c->idle_at = now(w);
    } else
        conn_kill(w, c);
    f->conn = NULL;
}

/* The body, as it is framed: 1 when something moved, 0 not, -1 when the fetch ended badly. */
static int h1_body(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f, int eof)
{
    int moved = 0;
    for (;;) {
        unsigned long avail = c->in_len - c->in_at, n, took;
        if (f->framing == FR_NONE || (f->framing == FR_LENGTH && !f->left)) {
            h1_release(w, c, f);
            wfn_body_end(w, f);
            return 1;
        }
        if (f->framing == FR_CHUNKED && f->chunk_state != 1) {
            /* 0: a size line; 2: the CRLF after the data; 3: trailer lines */
            unsigned long ln = line(c);
            long size;
            if (!ln) {
                if (c->in_len - c->in_at >= 1024) {
                    wfn_fail(w, f, WFN_E_PROTOCOL, "a bad chunk");
                    return -1;
                }
                break;
            }
            if (f->chunk_state == 0) {
                if ((size = parse_num((const char *)c->in + c->in_at, ln, 16, NULL)) < 0) {
                    wfn_fail(w, f, WFN_E_PROTOCOL, "a bad chunk size");
                    return -1;
                }
                f->left = (unsigned long)size;
                f->chunk_state = size ? 1 : 3;
            } else if (f->chunk_state == 2)
                f->chunk_state = 0;
            else if (ln <= 2) {                     /* the empty line after the trailers */
                c->in_at += ln;
                h1_release(w, c, f);
                wfn_body_end(w, f);
                return 1;
            }
            c->in_at += ln;
            moved = 1;
            continue;
        }
        if (!avail)
            break;
        n = f->framing == FR_CLOSE || avail < f->left ? avail : f->left;
        took = wfn_deliver(w, f, c->in + c->in_at, n);
        if (f->body_done)
            return -1;                              /* the decoder failed */
        if (!took)
            break;
        c->in_at += took;
        if (f->framing != FR_CLOSE)
            f->left -= took;
        if (f->framing == FR_CHUNKED && !f->left)
            f->chunk_state = 2;
        moved = 1;
    }
    if (eof) {
        if (f->framing == FR_CLOSE) {
            conn_kill(w, c);
            f->conn = NULL;
            wfn_body_end(w, f);
            return 1;
        }
        wfn_fail(w, f, WFN_E_CLOSED, "the connection closed before the body ended");
        return -1;
    }
    return moved;
}

/* ---- WebSocket ----------------------------------------------------------------- */

static void ws_opened(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    (void)w;
    c->ws = 1;
    f->state = F_WS;
    f->keep = 0;
    f->hflags |= WFN_H_NOBODY;
}

/* One frame of ours: header, mask and masked payload. */
static int ws_frame(struct wfn *w, struct wfn_conn *c, unsigned op, int fin, const unsigned char *p, unsigned long n)
{
    unsigned char h[14], mask[4], buf[256];
    unsigned long hl = 2, i, done = 0;
    h[0] = (unsigned char)((fin ? 0x80 : 0) | op);
    if (n < 126)
        h[1] = (unsigned char)(0x80 | n);
    else if (n < 65536) {
        h[1] = 0x80 | 126;
        h[2] = (unsigned char)(n >> 8), h[3] = (unsigned char)n;
        hl = 4;
    } else {
        h[1] = 0x80 | 127;
        h[2] = h[3] = h[4] = h[5] = 0;
        h[6] = (unsigned char)(n >> 24), h[7] = (unsigned char)(n >> 16), h[8] = (unsigned char)(n >> 8), h[9] = (unsigned char)n;
        hl = 10;
    }
    w->io.random(w->io.env, mask, 4);
    memcpy(h + hl, mask, 4);
    if (wfn_out(w, c, h, hl + 4))
        return -1;
    while (done < n) {
        unsigned long run = n - done < sizeof buf ? n - done : sizeof buf;
        for (i = 0; i < run; i++)
            buf[i] = p[done + i] ^ mask[(done + i) & 3];
        if (wfn_out(w, c, buf, run))
            return -1;
        done += run;
    }
    return 0;
}

static int ws_record(struct wfn_fetch *f, unsigned flags, const unsigned char *p, unsigned long n)
{
    unsigned char h[WS_RECORD];
    if (f->ring.size - f->ring.len < WS_RECORD + n)
        return -1;
    h[0] = (unsigned char)flags;
    h[1] = (unsigned char)(n >> 24), h[2] = (unsigned char)(n >> 16), h[3] = (unsigned char)(n >> 8), h[4] = (unsigned char)n;
    ring_put(&f->ring, h, WS_RECORD);
    ring_put(&f->ring, p, n);
    return 0;
}

/* Frames from the server into records in the ring: 1 when something moved. */
static int ws_read(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    int moved = 0;
    for (;;) {
        const unsigned char *p = c->in + c->in_at;
        unsigned long avail = c->in_len - c->in_at, hl = 2, len;
        unsigned op;
        int fin;
        if (f->ws_left) {                           /* the rest of a data frame's payload */
            unsigned long room = f->ring.size - f->ring.len, n = avail < f->ws_left ? avail : f->ws_left;
            if (room <= WS_RECORD || !avail)
                return moved;
            if (n > room - WS_RECORD)
                n = room - WS_RECORD;
            ws_record(f, f->ws_msg_op | (f->ws_fin && n == f->ws_left ? WFN_WS_FIN : 0), p, n);
            c->in_at += n;
            f->ws_left -= n;
            moved = 1;
            continue;
        }
        if (avail < 2)
            return moved;
        fin = p[0] & 0x80;
        op = p[0] & 15;
        len = p[1] & 127;
        if ((p[0] & 0x70) || (p[1] & 0x80) || (op > 2 && op < 8) || op > 10 || (op >= 8 && (len > 125 || !fin))) {
            wfn_conn_fail(w, c, WFN_E_PROTOCOL, "a bad WebSocket frame");
            return 1;
        }
        if (len == 126)
            hl = 4;
        else if (len == 127)
            hl = 10;
        if (avail < hl)
            return moved;
        if (len == 126)
            len = (unsigned long)p[2] << 8 | p[3];
        else if (len == 127) {
            if (p[2] | p[3] | p[4] | p[5] | (p[6] & 0x80)) {
                wfn_conn_fail(w, c, WFN_E_PROTOCOL, "a WebSocket frame over 2 GB");
                return 1;
            }
            len = (unsigned long)p[6] << 24 | (unsigned long)p[7] << 16 | (unsigned long)p[8] << 8 | p[9];
        }
        if (op >= 8) {
            if (avail < hl + len)
                return moved;
            if (op == WFN_WS_PING) {
                if (!f->ws_close_sent && ws_frame(w, c, WFN_WS_PONG, 1, p + hl, len))
                    return -1;
            } else {
                if (ws_record(f, op | WFN_WS_FIN, p + hl, len))
                    return moved;                   /* when the caller makes room */
                if (op == WFN_WS_CLOSE) {
                    if (!f->ws_close_sent) {
                        unsigned char code[2];
                        code[0] = len >= 2 ? p[hl] : 0x03, code[1] = len >= 2 ? p[hl + 1] : 0xe8;
                        ws_frame(w, c, WFN_WS_CLOSE, 1, code, 2);
                        f->ws_close_sent = 1;
                    }
                    f->close_code = len >= 2 ? (unsigned)p[hl] << 8 | p[hl + 1] : 1005;
                }
            }
            c->in_at += hl + len;
            moved = 1;
            continue;
        }
        if (op == WFN_WS_CONT) {
            if (!f->ws_msg_op) {
                wfn_conn_fail(w, c, WFN_E_PROTOCOL, "a WebSocket continuation with no message");
                return 1;
            }
        } else
            f->ws_msg_op = op;
        f->ws_fin = fin ? 1 : 0;
        c->in_at += hl;
        moved = 1;
        if (!len) {
            if (ws_record(f, f->ws_msg_op | (fin ? WFN_WS_FIN : 0), p, 0)) {
                c->in_at -= hl;
                return 0;
            }
            continue;
        }
        f->ws_left = len;
    }
}

/* ---- moving a connection on ---------------------------------------------------- */

static void tls_start(struct wfn *w, struct wfn_conn *c)
{
    c->state = C_TLS;
    c->tls = w->io.tls_new(w->io.env, c->sock, c->host, c->may_h2 ? "h2,http/1.1" : "http/1.1");
    if (!c->tls)
        wfn_conn_fail(w, c, WFN_E_TLS, "TLS could not be set up");
}

/* The connection is ready for requests. */
static void conn_ready(struct wfn *w, struct wfn_conn *c)
{
    struct wfn_fetch *f = c->owner;
    c->state = C_READY;
    if (c->h2) {
        c->owner = NULL;
        if (f)
            wfn_h2_attach(w, c, f);
    } else if (f)
        f->state = F_SEND;
}

/* An HTTP/1.1 or WebSocket connection with its fetch: 1 when something moved. */
static int h1_step(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    int moved = 0, eof = 0;
    long r;
    if (f->state == F_SEND && !f->sent_request) {
        if (write_request(w, c, f)) {
            wfn_fail(w, f, WFN_E_HOST, "no memory");
            return 1;
        }
        f->sent_request = 1;
        if (f->connect_phase)
            f->end_sent = 1;
        moved = 1;
    }
    if (f->state == F_SEND && !f->connect_phase && !f->end_sent) {
        int up = h1_upload(w, c, f);
        if (up < 0) {
            wfn_fail(w, f, WFN_E_HOST, "no memory");
            return 1;
        }
        moved |= up;
    }
    if (wfn_flush(w, c)) {
        wfn_conn_fail(w, c, WFN_E_CLOSED, f->got_bytes ? "the connection closed" : "the connection closed during the upload");
        return 1;
    }
    for (;;) {
        int step = 0;
        if (c->state == C_DEAD || c->owner != f)
            return 1;
        r = c->in_len - c->in_at < sizeof c->in ? wfn_conn_read(w, c) : WFN_AGAIN;
        if (r > 0)
            step = 1;
        else if (r == 0)
            eof = 1;
        else if (r == WFN_FAIL) {
            wfn_conn_fail(w, c, WFN_E_CLOSED, "the connection failed");
            return 1;
        }
        if (c->in_len > c->in_at && !f->got_bytes)
            f->got_bytes = 1;
        if (f->state == F_WS) {
            int m = ws_read(w, c, f);
            if (m < 0) {
                wfn_conn_fail(w, c, WFN_E_HOST, "no memory");
                return 1;
            }
            if (c->state == C_DEAD)
                return 1;
            step |= m;
            if (eof) {
                f->body_done = 1;
                if (!f->close_code)
                    f->error = WFN_E_CLOSED;
                conn_kill(w, c);
                f->conn = NULL;
                return 1;
            }
        } else if (!f->head_ready && !f->discard && (f->state == F_SEND || f->state == F_HEAD)) {
            /* The head, a byte at a time into hbuf, to its empty line. */
            while (c->in_at < c->in_len) {
                char ch = (char)c->in[c->in_at++];
                if (!f->hbuf && !(f->hbuf = zalloc(w, WFN_MAX_HEAD))) {
                    wfn_fail(w, f, WFN_E_HOST, "no memory");
                    return 1;
                }
                if (f->hbuf_len >= WFN_MAX_HEAD) {
                    wfn_fail(w, f, WFN_E_PROTOCOL, "the answer's head was over 64 KB");
                    return 1;
                }
                f->hbuf[f->hbuf_len++] = ch;
                step = 1;
                if (ch == '\n' && f->hbuf_len >= 2
                    && (f->hbuf[f->hbuf_len - 2] == '\n'
                        || (f->hbuf_len >= 3 && f->hbuf[f->hbuf_len - 2] == '\r' && f->hbuf[f->hbuf_len - 3] == '\n'))) {
                    int h = h1_head(w, c, f);
                    if (h < 0 || c->state == C_DEAD)
                        return 1;
                    if (h == 0)
                        break;
                }
            }
            if (f->state == F_CONNECTING || (f->state == F_SEND && !f->sent_request))
                return 1;                           /* the tunnel is up: TLS or the request next */
        }
        if ((f->state == F_BODY || f->discard) && f->conn == c) {
            int b = h1_body(w, c, f, eof);
            if (b)
                return 1;
        } else if (eof && f->conn == c && f->state != F_WS) {
            if (!f->got_bytes && c->reused && f->retries < MAX_RETRIES && retry_safe(f)) {
                conn_kill(w, c);
                f->retries++;
                wfn_requeue(w, f);
            } else
                wfn_fail(w, f, WFN_E_CLOSED, f->head_ready ? "the connection closed" : "the connection closed before the answer");
            return 1;
        }
        moved |= step;
        if (!step || r <= 0)
            break;
    }
    if (f->conn == c && f->state == F_SEND && f->end_sent && !f->head_ready)
        f->state = F_HEAD;
    return moved;
}

static int conn_step(struct wfn *w, struct wfn_conn *c)
{
    int r;
    switch (c->state) {
    case C_CONNECT:
        r = w->io.connected(w->io.env, c->sock);
        if (r == WFN_AGAIN || r == WFN_AGAIN_W)
            return 0;
        if (r) {
            wfn_conn_fail(w, c, WFN_E_CONNECT, "no connection: refused, or no route");
            return 1;
        }
        if (c->owner && c->owner->tunnel) {
            c->state = C_TUNNEL;
            c->owner->connect_phase = 1;
            c->owner->state = F_SEND;
        } else if (c->use_tls)
            tls_start(w, c);
        else
            conn_ready(w, c);
        return 1;
    case C_TLS:
        r = w->io.tls_handshake(w->io.env, c->tls);
        if (r == WFN_AGAIN || r == WFN_AGAIN_W) {
            c->want = r == WFN_AGAIN ? WFN_WANT_READ : WFN_WANT_WRITE;
            return 0;
        }
        {
            int verified = 0;
            c->tls_info_len = w->io.tls_info(w->io.env, c->tls, c->tls_info, sizeof c->tls_info, &verified);
            c->verified = verified ? 1 : 0;
        }
        if (r) {
            /* The handshake's own words go to each fetch waiting on it. */
            struct wfn_fetch *f;
            for (f = w->fetches; f; f = f->next)
                if (f->conn == c && f->info_len + c->tls_info_len < sizeof f->info) {
                    memcpy(f->info + f->info_len, c->tls_info, c->tls_info_len);
                    f->info_len += c->tls_info_len;
                }
            wfn_conn_fail(w, c, WFN_E_TLS, NULL);
            return 1;
        }
        if (c->may_h2 && !strcmp(w->io.tls_alpn(w->io.env, c->tls), "h2")) {
            if (wfn_h2_start(w, c)) {
                wfn_conn_fail(w, c, WFN_E_HOST, "no memory");
                return 1;
            }
        }
        conn_ready(w, c);
        return 1;
    case C_TUNNEL:
    case C_READY:
        if (c->h2)
            return wfn_h2_step(w, c);
        if (c->owner)
            return h1_step(w, c, c->owner);
        /* Kept, idle: the server closing it is seen here. */
        r = (int)wfn_conn_read(w, c);
        if (r != WFN_AGAIN) {
            conn_kill(w, c);
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ---- giving fetches connections ---------------------------------------------- */

static void conn_key(const struct wfn_fetch *f, char *key)
{
    /* Forwarded through an HTTP proxy: one key for the proxy, whatever the host. */
    unsigned long n = 0;
    key[n++] = f->forward ? 'F' : f->tunnel ? 'T' : 'D';
    key[n++] = f->use_tls ? 's' : 'p';
    key[n++] = f->flags & WFN_F_HTTP1 ? '1' : '2';
    if (!f->forward) {
        memcpy(key + n, f->host, strlen(f->host));
        n += strlen(f->host);
        key[n++] = '|';
        n += num(key + n, f->port, 10);
    }
    if (f->phost[0]) {
        key[n++] = '|';
        memcpy(key + n, f->phost, strlen(f->phost));
        n += strlen(f->phost);
        key[n++] = '|';
        n += num(key + n, f->pport, 10);
    }
    key[n] = 0;
}

/* Kept connections over the limits, and those idle too long, are closed. */
static void prune(struct wfn *w)
{
    struct wfn_conn *c, *oldest;
    unsigned long t = now(w);
    int idle;
    for (c = w->conns; c; c = c->next)
        if (c->state == C_READY && !c->owner && !wfn_h2_busy(c) && t - c->idle_at >= c->idle_ms)
            conn_kill(w, c);
    for (;;) {
        idle = 0;
        oldest = NULL;
        for (c = w->conns; c; c = c->next)
            if (c->state == C_READY && !c->owner && !wfn_h2_busy(c)) {
                idle++;
                if (!oldest || (long)(c->idle_at - oldest->idle_at) < 0)
                    oldest = c;
            }
        if (idle <= KEEP_MAX || !oldest)
            break;
        conn_kill(w, oldest);
    }
}

static int assign(struct wfn *w, struct wfn_fetch *f)
{
    char key[sizeof ((struct wfn_conn *)0)->key];
    struct wfn_conn *c;
    int pending = 0, busy = 0, err = 0;
    long sock;
    conn_key(f, key);
    if (!f->ws) {
        for (c = w->conns; c; c = c->next) {
            if (c->state == C_DEAD || strcmp(c->key, key))
                continue;
            if (c->state == C_READY && c->h2) {
                if (wfn_h2_room(c)) {
                    f->conn = c;
                    f->reused = c->reused;
                    c->reused = 1;
                    wfn_h2_attach(w, c, f);
                    return 1;
                }
                pending = 1;
                continue;
            }
            if (c->state == C_READY && !c->owner && !c->ws && !f->retries) {
                if (wfn_conn_read(w, c) != WFN_AGAIN || c->in_len) {
                    conn_kill(w, c);                /* the server closed it meanwhile */
                    continue;
                }
                c->owner = f;
                c->reused = 1;
                f->conn = c;
                f->reused = 1;
                f->state = F_SEND;
                return 1;
            }
            if (c->state != C_READY && c->may_h2)
                pending = 1;
            else
                busy++;
        }
        if (pending || busy >= KEEP_PER_HOST)
            return 0;
    }
    if (w->nconn >= WFN_MAX_FETCHES + KEEP_MAX)
        return 0;
    if (!(c = zalloc(w, sizeof *c))) {
        wfn_fail(w, f, WFN_E_HOST, "no memory");
        return 1;
    }
    sock = w->io.connect(w->io.env, f->phost[0] ? f->phost : f->host, f->phost[0] ? f->pport : f->port, &err);
    if (sock < 0) {
        zfree(w, c);
        wfn_fail(w, f, err == WFN_E_DNS ? WFN_E_DNS : WFN_E_CONNECT,
                 err == WFN_E_DNS ? "the name did not resolve" : "no connection: refused, or no route");
        return 1;
    }
    c->sock = sock;
    c->state = C_CONNECT;
    c->use_tls = f->use_tls;
    c->may_h2 = f->use_tls && !f->ws && !(f->flags & WFN_F_HTTP1);
    c->forward = f->forward;
    c->idle_ms = KEEP_IDLE;
    memcpy(c->key, key, sizeof key);
    cpy(c->host, sizeof c->host, f->host, strlen(f->host));
    c->owner = f;
    c->next = w->conns;
    w->conns = c;
    w->nconn++;
    f->conn = c;
    f->state = F_CONNECTING;
    return 1;
}

/* ---- progress, and waiting ------------------------------------------------------ */

static void progress(struct wfn *w)
{
    int round, changed;
    struct wfn_fetch *f;
    struct wfn_conn *c, *next;
    for (round = 0; round < 64; round++) {
        changed = 0;
        for (f = w->fetches; f; f = f->next)
            if (f->state == F_WAIT)
                changed |= assign(w, f);
        for (c = w->conns; c; c = c->next)
            if (c->state != C_DEAD)
                changed |= conn_step(w, c);
        for (f = w->fetches; f; f = f->next) {
            if (f->raw.len)
                changed |= drain(w, f);
            if (f->end_recv && !f->body_done && !f->raw.len && !(f->dec && dec_finish(w, f))) {
                f->body_done = 1;
                f->state = F_DONE;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    /* Timeouts: the server has been silent too long (not while the caller lets the ring fill). */
    {
        unsigned long t = now(w);
        for (f = w->fetches; f; f = f->next) {
            if (f->state == F_DONE || f->state == F_WS)
                continue;
            if (f->ring.len == f->ring.size || (f->conn && f->conn->blocked_w))
                f->deadline = t + f->timeout_ms;
            else if (passed(t, f->deadline))
                wfn_fail(w, f, WFN_E_TIMEOUT, "no answer within the timeout");
        }
    }
    prune(w);
    for (c = w->conns; c; c = next) {
        next = c->next;
        if (c->state == C_DEAD) {
            for (f = w->fetches; f; f = f->next)
                if (f->conn == c)
                    f->conn = NULL;
            conn_free(w, c);
        }
    }
}

/* Waits at most ms for a socket to be ready: WFN_BROKEN when told to stop. */
static int block(struct wfn *w, unsigned long ms)
{
    long socks[WFN_MAX_FETCHES + KEEP_MAX];
    unsigned char want[WFN_MAX_FETCHES + KEEP_MAX], ready[WFN_MAX_FETCHES + KEEP_MAX];
    struct wfn_conn *c;
    struct wfn_fetch *f;
    unsigned long t = now(w);
    int n = 0;
    for (f = w->fetches; f; f = f->next)
        if (f->state != F_DONE && f->state != F_WS) {
            unsigned long left = (long)(f->deadline - t) > 0 ? f->deadline - t : 0;
            if (left < ms)
                ms = left;
        }
    for (c = w->conns; c && n < (int)(sizeof socks / sizeof socks[0]); c = c->next) {
        unsigned char m = 0;
        if (c->state == C_DEAD)
            continue;
        if (c->tls && c->state != C_TLS && c->state != C_CONNECT && w->io.tls_pending(w->io.env, c->tls) > 0)
            return 0;
        if (c->state == C_CONNECT)
            m = WFN_WANT_WRITE;
        else if (c->state == C_TLS)
            m = c->want ? c->want : WFN_WANT_READ;
        else {
            if (c->blocked_w)
                m |= WFN_WANT_WRITE;
            if (c->in_len < sizeof c->in)
                m |= WFN_WANT_READ;
        }
        socks[n] = c->sock;
        want[n++] = m;
    }
    return w->io.wait(w->io.env, socks, want, ready, n, ms);
}

/* Runs until done(f) or the wait is over. */
static void wait_for(struct wfn *w, struct wfn_fetch *f, unsigned long wait, int (*done)(const struct wfn_fetch *))
{
    unsigned long start = now(w);
    if (wait > MAX_WAIT)
        wait = MAX_WAIT;
    for (;;) {
        unsigned long el;
        progress(w);
        if (done(f))
            return;
        el = now(w) - start;
        if (el >= wait || block(w, wait - el) == WFN_BROKEN)
            return;
    }
}

static int head_there(const struct wfn_fetch *f)
{
    return f->head_ready || f->error;
}

static int body_there(const struct wfn_fetch *f)
{
    return (f->ring.len && f->head_ready) || f->body_done;
}

static int ws_there(const struct wfn_fetch *f)
{
    return f->rec_left || f->ring.len || f->body_done || (f->state != F_WS && head_there(f));
}

static int send_room(const struct wfn_fetch *f)
{
    return f->up_queued < WFN_UPLOAD_MAX || f->body_done;
}

static int ws_room(const struct wfn_fetch *f)
{
    return !f->conn || out_waiting(f->conn) < WFN_OUT_MAX;
}

/* Moves every fetch on; then waits at most ms for a socket to be ready (or the wait to be broken), and
 * moves them on again. For a task with many fetches: pump, then ask each with a wait of 0. */
void wfn_pump(struct wfn *w, unsigned long ms)
{
    progress(w);
    if (ms) {
        block(w, ms > MAX_WAIT ? MAX_WAIT : ms);
        progress(w);
    }
}

/* ---- OPEN ------------------------------------------------------------------- */

static void pct_decode(char *out, unsigned long room, const char *s, unsigned long n)
{
    unsigned long o = 0, i;
    for (i = 0; i < n && o + 1 < room; i++) {
        unsigned long used;
        long v;
        if (s[i] == '%' && i + 2 < n + 0 && (v = parse_num(s + i + 1, 2, 16, &used)) >= 0 && used == 2) {
            out[o++] = (char)v;
            i += 2;
        } else
            out[o++] = s[i];
    }
    out[o] = 0;
}

/* scheme://[user[:pass]@]host[:port][path]: 0, or -1. kind: 1 http, 2 https, 3 ws, 4 wss. */
static int parse_url(const char *u, unsigned long n, int *kind, char *host, unsigned *port, char *user, char *pass,
                     const char **path, unsigned long *pathlen)
{
    const char *p = u, *end = u + n, *auth, *at, *h, *he, *q;
    long pv;
    unsigned long used;
    if (n > 7 && wfn_ieq(p, 7, "http://"))
        *kind = 1, p += 7;
    else if (n > 8 && wfn_ieq(p, 8, "https://"))
        *kind = 2, p += 8;
    else if (n > 5 && wfn_ieq(p, 5, "ws://"))
        *kind = 3, p += 5;
    else if (n > 6 && wfn_ieq(p, 6, "wss://"))
        *kind = 4, p += 6;
    else
        return -1;
    auth = p;
    while (p < end && *p != '/' && *p != '?' && *p != '#')
        p++;
    for (at = p; at > auth && at[-1] != '@'; at--)
        ;
    user[0] = pass[0] = 0;
    if (at > auth) {
        const char *colon = auth;
        while (colon < at - 1 && *colon != ':')
            colon++;
        pct_decode(user, WFN_CRED_ROOM, auth, (unsigned long)(colon - auth));
        if (colon < at - 1)
            pct_decode(pass, WFN_CRED_ROOM, colon + 1, (unsigned long)(at - 1 - colon - 1));
    }
    h = at;
    if (h < p && *h == '[') {
        for (he = h; he < p && *he != ']'; he++)
            ;
        if (he == p)
            return -1;
        if (cpy(host, WFN_HOST_ROOM, h + 1, (unsigned long)(he - h - 1)))
            return -1;
        q = he + 1;
    } else {
        for (he = h; he < p && *he != ':'; he++)
            ;
        if (cpy(host, WFN_HOST_ROOM, h, (unsigned long)(he - h)))
            return -1;
        q = he;
    }
    if (!host[0])
        return -1;
    *port = (*kind == 2 || *kind == 4) ? 443 : 80;
    if (q < p) {
        if (*q != ':' || (pv = parse_num(q + 1, (unsigned long)(p - q - 1), 10, &used)) <= 0 || pv > 65535
            || used != (unsigned long)(p - q - 1))
            return -1;
        *port = (unsigned)pv;
    }
    *path = p;
    for (q = p; q < end && *q != '#'; q++)
        ;
    *pathlen = (unsigned long)(q - p);
    return 0;
}

/* Whether host is in a no_proxy list ("*", or names and domains, by commas). */
static int no_proxy(const char *list, const char *host)
{
    const char *p = list;
    unsigned long hl = strlen(host);
    while (*p) {
        const char *s, *e;
        unsigned long n;
        while (*p == ',' || *p == ' ')
            p++;
        s = p;
        while (*p && *p != ',' && *p != ' ')
            p++;
        e = p;
        if (e - s == 1 && *s == '*')
            return 1;
        if (s < e && *s == '.')
            s++;
        {
            const char *c = s;
            while (c < e && *c != ':')
                c++;
            e = c;
        }
        n = (unsigned long)(e - s);
        if (n && n <= hl) {
            unsigned long i;
            int same = 1;
            for (i = 0; i < n; i++)
                if (lower((unsigned char)host[hl - n + i]) != lower((unsigned char)s[i]))
                    same = 0;
            if (same && (n == hl || host[hl - n - 1] == '.'))
                return 1;
        }
    }
    return 0;
}

/* The proxy for this fetch: from the request's ":proxy" line, else the environment. 0, or -1 when bad. */
static int choose_proxy(struct wfn *w, struct wfn_fetch *f, const char *line, unsigned long ll, int kind)
{
    char url[512], list[512];
    const char *path;
    unsigned long pl, n;
    int pk;
    if (line) {
        if (!ll || wfn_ieq(line, ll, "none") || wfn_ieq(line, ll, "direct"))
            return 0;
        if (cpy(url, sizeof url, line, ll))
            return -1;
    } else {
        /* http_proxy for http:// and ws://, https_proxy for https:// and wss://, then all_proxy */
        static const char names[] = "http_proxy\0HTTP_PROXY\0all_proxy\0ALL_PROXY\0https_proxy\0HTTPS_PROXY\0all_proxy\0ALL_PROXY";
        const char *name = names + ((kind == 2 || kind == 4) ? 42 : 0);
        int i;
        long got = -1;
        if (f->flags & WFN_F_NOPROXY || !w->io.getenv)
            return 0;
        for (i = 0; i < 4 && got <= 0; i++, name += strlen(name) + 1)
            got = w->io.getenv(w->io.env, name, url, sizeof url);
        if (got <= 0)
            return 0;
        got = w->io.getenv(w->io.env, "no_proxy", list, sizeof list);
        if (got <= 0)
            got = w->io.getenv(w->io.env, "NO_PROXY", list, sizeof list);
        if (got > 0 && no_proxy(list, f->host))
            return 0;
    }
    n = strlen(url);
    if (!strstr(url, "://")) {                      /* "host:port" means http:// */
        if (n + 7 >= sizeof url)
            return -1;
        memmove(url + 7, url, n + 1);
        memcpy(url, "http://", 7);
        n += 7;
    }
    if (parse_url(url, n, &pk, f->phost, &f->pport, f->puser, f->ppass, &path, &pl) || pk != 1)
        return -1;
    {
        /* No port given: 1080, as curl has it. */
        const char *hp = strchr(url, '@') ? strchr(url, '@') + 1 : url + 7;
        const char *c = hp[0] == '[' ? strchr(hp, ']') : hp;
        if (!c || !strchr(c, ':'))
            f->pport = 1080;
    }
    if (kind == 1)
        f->forward = 1;
    else
        f->tunnel = 1;
    return 0;
}

static long op_open(struct wfn *w, int ws, const unsigned long extra[4], struct wfn_buf buf[4], unsigned long *result)
{
    const char *req = (const char *)buf[0].in, *end, *p, *sp, *url, *path, *proxy = NULL;
    unsigned long len = buf[0].length, pl, ul, hl = 0, proxy_len = 0;
    struct wfn_fetch *f;
    int kind;
    char *h;
    if (!req || !len || len > WFN_MAX_HEAD)
        return WFN_BADREQUEST;
    if (w->nfetch >= WFN_MAX_FETCHES)
        return WFN_BUSY;
    if (!(f = zalloc(w, sizeof *f)))
        return WFN_BUSY;
    f->flags = extra[0];
    end = req + len;
    /* "METHOD URL" */
    for (p = req; p < end && *p != '\n'; p++)
        ;
    for (sp = req; sp < p && *sp != ' '; sp++)
        if (!tchar((unsigned char)*sp))
            goto bad;
    if (sp == req || sp == p || cpy(f->method, sizeof f->method, req, (unsigned long)(sp - req)))
        goto bad;
    url = sp + 1;
    for (ul = (unsigned long)(p - url); ul && (url[ul - 1] == '\r' || url[ul - 1] == ' '); ul--)
        ;
    if (parse_url(url, ul, &kind, f->host, &f->port, f->user, f->pass, &path, &pl) || (ws ? kind < 3 : kind > 2))
        goto bad;
    f->use_tls = kind == 2 || kind == 4;
    f->ws = (unsigned char)ws;
    if (!(f->target = zalloc(w, pl + 2)))
        goto busy;
    if (!pl || *path != '/')
        f->target[0] = '/';
    memcpy(f->target + (!pl || *path != '/'), path, pl);
    /* The header lines, CRLF: the connection's own dropped; ":proxy URL" is ours. */
    if (!(f->headers = zalloc(w, (unsigned long)(end - p) * 2 + 2)))
        goto busy;
    h = f->headers;
    while (p < end) {
        const char *s = ++p, *e, *colon;
        unsigned long nl;
        while (p < end && *p != '\n')
            p++;
        e = p > s && p[-1] == '\r' ? p - 1 : p;
        if (e == s)
            continue;
        if (*s == ':') {
            if (e - s > 6 && wfn_ieq(s, 6, ":proxy") && (s[6] == ' ' || s[6] == ':')) {
                proxy = s + 7;
                while (proxy < e && *proxy == ' ')
                    proxy++;
                proxy_len = (unsigned long)(e - proxy);
            }
            continue;
        }
        for (colon = s; colon < e && *colon != ':'; colon++)
            if (!tchar((unsigned char)*colon))
                goto bad;
        if (colon == s || colon == e)
            goto bad;
        nl = (unsigned long)(colon - s);
        if (hop_header(s, nl) || wfn_ieq(s, nl, "content-length")
            || ((f->flags & WFN_F_DECODE) && wfn_ieq(s, nl, "accept-encoding"))
            || (ws && (wfn_ieq(s, nl, "sec-websocket-key") || wfn_ieq(s, nl, "sec-websocket-version"))))
            continue;
        if (wfn_ieq(s, nl, "user-agent"))
            f->has_ua = 1;
        memcpy(h + hl, s, (unsigned long)(e - s));
        hl += (unsigned long)(e - s);
        h[hl++] = '\r';
        h[hl++] = '\n';
    }
    f->headers_len = hl;
    if (choose_proxy(w, f, proxy, proxy_len, kind))
        goto bad;
    /* the body, or its first part */
    if (f->flags & WFN_F_BODY) {
        f->body_length = extra[1];
        if (buf[1].in && buf[1].length) {
            if (f->body_length != WFN_LENGTH_UNKNOWN && buf[1].length > f->body_length)
                goto bad;
            if (up_add(w, f, buf[1].in, buf[1].length))
                goto busy;
        }
        f->keep_body = f->body_length != WFN_LENGTH_UNKNOWN && f->up_given == f->body_length;
    }
    f->timeout_ms = (extra[2] ? (extra[2] > TIMEOUT_MAX ? TIMEOUT_MAX : extra[2]) : TIMEOUT_DEF) * 1000;
    if (!ring_new(w, &f->ring))
        goto busy;
    if (ws) {
        unsigned char k[16];
        w->io.random(w->io.env, k, 16);
        wfn_base64(k, 16, f->ws_key);
    }
    wfn_info(f, "transport", "native");
    f->deadline = now(w) + f->timeout_ms;
    f->touched = now(w);
    f->state = F_WAIT;
    if (!++w->next_handle)
        w->next_handle = 1;
    f->handle = w->next_handle;
    f->next = w->fetches;
    w->fetches = f;
    w->nfetch++;
    *result = f->handle;
    progress(w);                                    /* start connecting now */
    return WFN_OK;
bad:
    {
        struct wfn_fetch *g = f;
        zfree(w, g->target);
        zfree(w, g->headers);
        while (g->up_head) {
            struct wfn_piece *n = g->up_head->next;
            zfree(w, g->up_head);
            g->up_head = n;
        }
        zfree(w, g);
    }
    return WFN_BADREQUEST;
busy:
    {
        struct wfn_fetch *g = f;
        zfree(w, g->target);
        zfree(w, g->headers);
        while (g->up_head) {
            struct wfn_piece *n = g->up_head->next;
            zfree(w, g->up_head);
            g->up_head = n;
        }
        zfree(w, g->ring.p);
        zfree(w, g);
    }
    return WFN_BUSY;
}

/* ---- the other ops ----------------------------------------------------------- */

static struct wfn_fetch *find(struct wfn *w, unsigned long handle)
{
    struct wfn_fetch *f;
    for (f = w->fetches; f; f = f->next)
        if (f->handle == handle)
            return f;
    return NULL;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v;
}

static void fetch_free(struct wfn *w, struct wfn_fetch *f)
{
    struct wfn_fetch **pp = &w->fetches;
    if (f->conn) {
        if (f->conn->h2)
            wfn_h2_detach(w, f->conn, f, 1);
        else if (f->conn->owner == f)
            conn_kill(w, f->conn);
    }
    while (*pp && *pp != f)
        pp = &(*pp)->next;
    if (*pp)
        *pp = f->next;
    while (f->up_head) {
        struct wfn_piece *n = f->up_head->next;
        zfree(w, f->up_head);
        f->up_head = n;
    }
    if (f->dec)
        w->io.dec_free(w->io.env, f->dec);
    zfree(w, f->target);
    zfree(w, f->headers);
    zfree(w, f->auth);
    zfree(w, f->pauth);
    zfree(w, f->hbuf);
    zfree(w, f->htext);
    zfree(w, f->ring.p);
    zfree(w, f->raw.p);
    zfree(w, f);
    w->nfetch--;
}

long wfn_call(struct wfn *w, unsigned op, unsigned long arg, const unsigned long extra[4], struct wfn_buf buf[4],
              unsigned long *result, unsigned long *aux)
{
    static const unsigned long none[4] = { 0, 0, 0, 0 };
    struct wfn_fetch *f = NULL;
    unsigned long n;
    *result = *aux = 0;
    if (!extra)
        extra = none;
    if (op == WFN_OPEN || op == WFN_WS_OPEN)
        return op_open(w, op == WFN_WS_OPEN, extra, buf, result);
    if (op == WFN_STATS) {
        progress(w);
        *result = (unsigned long)w->nfetch;
        *aux = (unsigned long)w->nconn;
        return WFN_OK;
    }
    if (!(f = find(w, arg)))
        return WFN_BADREQUEST;
    f->touched = now(w);
    switch (op) {
    case WFN_SEND:
        if (!(f->flags & WFN_F_BODY) || f->up_last || (buf[0].length && !buf[0].in))
            return WFN_BADREQUEST;
        if (f->body_length != WFN_LENGTH_UNKNOWN && f->up_given + buf[0].length > f->body_length)
            return WFN_BADREQUEST;
        wait_for(w, f, extra[1], send_room);
        if (f->body_done) {
            *result = buf[0].length;                /* the fetch is over: nothing more is sent */
            return WFN_OK;
        }
        if (!send_room(f))
            return WFN_OK;
        if (up_add(w, f, buf[0].in, buf[0].length))
            return WFN_BUSY;
        if (extra[0] & 1)
            f->up_last = 1;
        f->keep_body = 0;
        *result = buf[0].length;
        progress(w);
        return WFN_OK;
    case WFN_HEAD:
        if (!buf[0].out)
            return WFN_BADREQUEST;
        wait_for(w, f, extra[0], head_there);
        if (!head_there(f))
            return WFN_OK;
        n = 32 + f->hlen + f->info_len;
        if (buf[0].length < n) {
            *result = n;
            return WFN_TOOSMALL;
        }
        put32(buf[0].out, 0x4f574631UL);
        put32(buf[0].out + 4, f->head_ready ? 0 : f->error);
        put32(buf[0].out + 8, f->head_ready ? f->status : 0);
        put32(buf[0].out + 12, f->head_ready ? f->hflags : 0);
        put32(buf[0].out + 16, f->head_ready ? f->length : 0);
        put32(buf[0].out + 20, f->head_ready ? f->hlen : 0);
        put32(buf[0].out + 24, f->info_len);
        put32(buf[0].out + 28, f->head_ready ? f->version : 0);
        if (f->head_ready)
            memcpy(buf[0].out + 32, f->htext, f->hlen);
        memcpy(buf[0].out + 32 + (f->head_ready ? f->hlen : 0), f->info, f->info_len);
        buf[0].written = 32 + (f->head_ready ? f->hlen : 0) + f->info_len;
        *result = f->head_ready ? f->status : 0;
        *aux = f->head_ready ? 0 : f->error;
        return WFN_OK;
    case WFN_READ:
        if (!buf[0].out)
            return WFN_BADREQUEST;
        if (f->ws && (f->state == F_WS || (!f->head_ready && !f->error))) {
            *aux = 1 | (WFN_E_PROTOCOL << 8);       /* a WebSocket's messages come through WS_RECV */
            return WFN_OK;
        }
        wait_for(w, f, extra[0], body_there);
        n = f->head_ready ? ring_get(&f->ring, buf[0].out, buf[0].length) : 0;
        buf[0].written = n;
        *result = n;
        if (n && f->conn && f->conn->h2 && !f->dec)
            wfn_h2_consumed(w, f->conn, f, n);
        if (f->body_done && !f->ring.len && !f->raw.len)
            *aux = 1 | (f->error << 8);
        return WFN_OK;
    case WFN_WS_SEND:
        if (!f->ws || (buf[0].length && !buf[0].in) || (extra[0] & 15) > 10)
            return WFN_BADREQUEST;
        wait_for(w, f, extra[1], ws_room);
        if (f->state != F_WS || !f->conn || f->ws_close_sent) {
            *aux = 1 | (f->error << 8);
            return WFN_OK;
        }
        if (!ws_room(f))
            return WFN_OK;
        {
            unsigned op8 = (unsigned)(extra[0] & 15), more = (extra[0] & WFN_WS_MORE) != 0;
            if (op8 < 8) {
                if (f->ws_more_out)
                    op8 = WFN_WS_CONT;
                f->ws_more_out = (unsigned char)more;
            } else if (buf[0].length > 125)
                return WFN_BADREQUEST;
            if (ws_frame(w, f->conn, op8, op8 >= 8 || !more, buf[0].in, buf[0].length))
                return WFN_BUSY;
            if (op8 == WFN_WS_CLOSE)
                f->ws_close_sent = 1;
        }
        *result = buf[0].length;
        progress(w);
        return WFN_OK;
    case WFN_WS_RECV:
        if (!f->ws || !buf[0].out)
            return WFN_BADREQUEST;
        wait_for(w, f, extra[0], ws_there);
        if (f->state != F_WS) {
            /* Not open: still on its way (nothing yet), or refused (the head says why). */
            if (f->body_done || f->head_ready)
                *aux = WFN_WS_END | ((f->error ? f->error : WFN_E_PROTOCOL) << 8);
            return WFN_OK;
        }
        if (!f->rec_left && f->ring.len >= WS_RECORD) {
            unsigned char hd[WS_RECORD];
            ring_get(&f->ring, hd, WS_RECORD);
            f->rec_flags = hd[0];
            f->rec_left = (unsigned long)hd[1] << 24 | (unsigned long)hd[2] << 16 | (unsigned long)hd[3] << 8 | hd[4];
            if ((f->rec_flags & 15) == WFN_WS_CLOSE && f->rec_left >= 2) {
                ring_get(&f->ring, hd, 2);
                f->rec_left -= 2;
            }
            f->rec_flags |= 0x100;                  /* a record is being given */
        }
        if (f->rec_flags & 0x100) {
            n = ring_get(&f->ring, buf[0].out, f->rec_left < buf[0].length ? f->rec_left : buf[0].length);
            f->rec_left -= n;
            *result = n;
            buf[0].written = n;
            *aux = (f->rec_flags & 15) | (!f->rec_left ? f->rec_flags & WFN_WS_FIN : 0);
            if ((f->rec_flags & 15) == WFN_WS_CLOSE)
                *aux |= (unsigned long)f->close_code << 16;
            if (!f->rec_left)
                f->rec_flags = 0;
        }
        if (!f->rec_left && !f->ring.len && f->body_done)
            *aux |= WFN_WS_END | (f->error << 8);
        progress(w);
        return WFN_OK;
    case WFN_CANCEL:
        if (!f->body_done)
            wfn_fail(w, f, WFN_E_CANCELLED, "cancelled");
        progress(w);
        return WFN_OK;
    case WFN_CLOSE:
        fetch_free(w, f);
        progress(w);
        return WFN_OK;
    }
    return WFN_BADREQUEST;
}

/* ---- making and ending --------------------------------------------------------- */

struct wfn *wfn_new(const struct wfn_io *io, unsigned long window)
{
    struct wfn *w = io->alloc(io->env, sizeof *w);
    if (!w)
        return NULL;
    w->io = *io;
    w->window = window ? (window < 16384 ? 16384 : window) : 65536;
    return w;
}

void wfn_free(struct wfn *w)
{
    if (!w)
        return;
    while (w->fetches)
        fetch_free(w, w->fetches);
    while (w->conns)
        conn_free(w, w->conns);
    w->io.release(w->io.env, w);
}
