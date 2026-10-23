/*
 * wfnet: HTTP/2 (RFC 9113) for its HTTPS connections whose server chose
 * "h2" by ALPN. Each fetch is a stream; streams share the connection, up
 * to the server's MAX_CONCURRENT_STREAMS. Headers go through HPACK
 * (wfnet_hpack.c): ours as literals (with the static table's names and
 * Huffman where shorter, never adding to the server's table), the
 * server's with a dynamic table of 4 KB.
 *
 * Flow control: each stream's window is the fetch's ring, given back as
 * the caller reads it, so the server never sends more than the fetch can
 * hold. The connection's window is larger, and given back as data comes.
 * Server push is turned off. GOAWAY sends the streams the server did not
 * take to a new connection, when they can be sent again; RST_STREAM ends
 * one stream, or (REFUSED_STREAM) sends it again.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "openweb/wfnet.h"
#include "wfnet_int.h"

/* frame types */
enum { T_DATA, T_HEADERS, T_PRIORITY, T_RST, T_SETTINGS, T_PUSH, T_PING, T_GOAWAY, T_WINDOW, T_CONT };
/* flags */
#define FL_END_STREAM  0x1
#define FL_ACK         0x1
#define FL_END_HEADERS 0x4
#define FL_PADDED      0x8
#define FL_PRIORITY    0x20
/* error codes */
#define E_NO_ERROR      0
#define E_PROTOCOL      1
#define E_FLOW          3
#define E_FRAME_SIZE    6
#define E_REFUSED       7
#define E_CANCEL        8
#define E_COMPRESSION   9

#define TABLE_SIZE      4096UL
#define MAX_BLOCK       (64UL << 10)

struct wfn_h2 {
    struct wfn_hpack *dec;
    long send_win;                      /* what the server lets us send on the connection */
    unsigned long conn_win, credit;     /* our window for the connection, and bytes not yet given back */
    unsigned long max_frame, init_win, max_streams;
    wu32 next_sid, cont_sid;
    int goaway, attached, open;
    unsigned char cont_end;             /* the HEADERS that CONTINUATION goes on had END_STREAM */
    unsigned char *block;               /* a header block being put together */
    unsigned long block_len;
};

static void put32(unsigned char *p, wu32 v)
{
    p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v;
}

static wu32 get32(const unsigned char *p)
{
    return (wu32)p[0] << 24 | (wu32)p[1] << 16 | (wu32)p[2] << 8 | p[3];
}

static int frame(struct wfn *w, struct wfn_conn *c, unsigned type, unsigned flags, wu32 sid, const void *p,
                 unsigned long n)
{
    unsigned char h[9];
    h[0] = (unsigned char)(n >> 16), h[1] = (unsigned char)(n >> 8), h[2] = (unsigned char)n;
    h[3] = (unsigned char)type, h[4] = (unsigned char)flags;
    put32(h + 5, sid & 0x7fffffffUL);
    return wfn_out(w, c, h, 9) || (n && wfn_out(w, c, p, n)) ? -1 : 0;
}

static void window_update(struct wfn *w, struct wfn_conn *c, wu32 sid, unsigned long inc)
{
    unsigned char b[4];
    put32(b, (wu32)inc);
    frame(w, c, T_WINDOW, 0, sid, b, 4);
}

static void rst(struct wfn *w, struct wfn_conn *c, wu32 sid, wu32 code)
{
    unsigned char b[4];
    put32(b, code);
    frame(w, c, T_RST, 0, sid, b, 4);
}

int wfn_h2_start(struct wfn *w, struct wfn_conn *c)
{
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    unsigned char s[12];
    struct wfn_h2 *h = w->io.alloc(w->io.env, sizeof *h);
    if (!h)
        return -1;
    if (!(h->dec = wfn_hpack_new(&w->io, TABLE_SIZE))) {
        w->io.release(w->io.env, h);
        return -1;
    }
    h->send_win = 65535;
    h->max_frame = WFN_FRAME;
    h->init_win = 65535;
    h->max_streams = 100;
    h->next_sid = 1;
    h->conn_win = w->window * 16;
    c->h2s = h;
    c->h2 = 1;
    /* SETTINGS: no push; each stream's window the fetch's ring. Then the connection's window. */
    s[0] = 0, s[1] = 2, put32(s + 2, 0);
    s[6] = 0, s[7] = 4, put32(s + 8, (wu32)w->window);
    if (wfn_out(w, c, preface, sizeof preface - 1) || frame(w, c, T_SETTINGS, 0, 0, s, 12))
        return -1;
    window_update(w, c, 0, h->conn_win - 65535);
    return 0;
}

void wfn_h2_free(struct wfn *w, struct wfn_conn *c)
{
    struct wfn_h2 *h = c->h2s;
    if (!h)
        return;
    wfn_hpack_free(&w->io, h->dec);
    if (h->block)
        w->io.release(w->io.env, h->block);
    w->io.release(w->io.env, h);
    c->h2s = NULL;
}

int wfn_h2_room(const struct wfn_conn *c)
{
    const struct wfn_h2 *h = c->h2s;
    return h && !h->goaway && (unsigned long)h->attached < h->max_streams;
}

int wfn_h2_busy(const struct wfn_conn *c)
{
    return c->h2s && c->h2s->attached > 0;
}

void wfn_h2_attach(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    (void)w;
    f->conn = c;
    f->sid = 0;
    f->state = F_SEND;
    c->h2s->attached++;
}

void wfn_h2_detach(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f, int cancel)
{
    struct wfn_h2 *h = c->h2s;
    if (!h)
        return;
    if (f->sid && cancel && !(f->end_recv && f->end_sent) && c->state != C_DEAD)
        rst(w, c, f->sid, E_CANCEL);
    if (f->sid)
        h->open--;
    h->attached--;
    if (!h->attached)
        c->idle_at = w->io.now(w->io.env);
    f->sid = 0;
    f->conn = NULL;
}

void wfn_h2_consumed(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f, unsigned long n)
{
    if (!f->sid || f->end_recv || c->state == C_DEAD)
        return;
    f->rcredit += n;
    if (f->rcredit >= w->window / 4) {
        window_update(w, c, f->sid, f->rcredit);
        f->rcredit = 0;
    }
}

static struct wfn_fetch *stream(struct wfn *w, struct wfn_conn *c, wu32 sid)
{
    struct wfn_fetch *f;
    for (f = w->fetches; f; f = f->next)
        if (f->conn == c && f->sid == sid)
            return f;
    return NULL;
}

/* A connection error: GOAWAY, and every stream fails or goes elsewhere. */
static int conn_error(struct wfn *w, struct wfn_conn *c, wu32 code, const char *why)
{
    unsigned char b[8];
    put32(b, 0);
    put32(b + 4, code);
    frame(w, c, T_GOAWAY, 0, 0, b, 8);
    wfn_flush(w, c);
    wfn_conn_fail(w, c, WFN_E_PROTOCOL, why);
    return 1;
}

/* ---- sending ---------------------------------------------------------------- */

static unsigned long field(struct wfn_conn *c, unsigned char *out, unsigned long room, const char *name,
                           unsigned long nl, const char *value, unsigned long vl)
{
    (void)c;
    return wfn_hpack_encode(NULL, name, nl, value, vl, 0, 1, out, room);
}

/* The request's HEADERS (and CONTINUATIONs): 0, or -1. */
static int send_headers(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    struct wfn_h2 *h = c->h2s;
    unsigned long room = f->headers_len + strlen(f->target) + 2 * WFN_HOST_ROOM + 2048 + (f->auth ? strlen(f->auth) : 0);
    unsigned long n = 0, at, k;
    unsigned char *b = w->io.alloc(w->io.env, room);
    char *p = f->headers, *end = f->headers + f->headers_len;
    char a[WFN_HOST_ROOM + 16], num[16];
    int end_stream, bad = 0, v6 = strchr(f->host, ':') != NULL;
    if (!b)
        return -1;
    /* :authority as Host would be */
    at = 0;
    if (v6)
        a[at++] = '[';
    memcpy(a + at, f->host, strlen(f->host));
    at += strlen(f->host);
    if (v6)
        a[at++] = ']';
    if (f->port != 443) {
        unsigned long v = f->port, d = 1;
        a[at++] = ':';
        while (v / d >= 10)
            d *= 10;
        for (; d; d /= 10)
            a[at++] = (char)('0' + v / d % 10);
    }
    a[at] = 0;
#define FIELD(nm, nl_, v_, vl_) do { if (!(k = field(c, b + n, room - n, nm, nl_, v_, vl_))) bad = 1; else n += k; } while (0)
    FIELD(":method", 7, f->method, strlen(f->method));
    FIELD(":scheme", 7, "https", 5);
    FIELD(":authority", 10, a, at);
    FIELD(":path", 5, f->target, strlen(f->target));
    while (p < end && !bad) {
        char *e = p, *colon;
        const char *v;
        while (e < end && *e != '\r')
            e++;
        for (colon = p; colon < e && *colon != ':'; colon++)
            if (*colon >= 'A' && *colon <= 'Z')
                *colon += 32;               /* HTTP/2 names are lower case */
        for (v = colon + 1; v < e && (*v == ' ' || *v == '\t'); v++)
            ;
        if (colon < e)
            FIELD(p, (unsigned long)(colon - p), v, (unsigned long)(e - v));
        p = e + 2;
    }
    if (!f->has_ua)
        FIELD("user-agent", 10, "OpenWeb-Fetch/2", 15);
    if (f->flags & WFN_F_DECODE) {
        const char *ae = wfn_accept(w);
        FIELD("accept-encoding", 15, ae, strlen(ae));
    }
    if (f->auth)
        FIELD("authorization", 13, f->auth, strlen(f->auth));
    if ((f->flags & WFN_F_BODY) && f->body_length != WFN_LENGTH_UNKNOWN) {
        unsigned long v = f->body_length, d = 1, m = 0;
        while (v / d >= 10)
            d *= 10;
        for (; d; d /= 10)
            num[m++] = (char)('0' + v / d % 10);
        FIELD("content-length", 14, num, m);
    }
#undef FIELD
    if (bad) {
        w->io.release(w->io.env, b);
        return -1;
    }
    end_stream = wfn_up_done(f) && !f->up_queued;
    f->sid = h->next_sid;
    h->next_sid += 2;
    h->open++;
    f->swin = (long)h->init_win;
    f->rcredit = 0;
    f->sent_request = 1;
    for (at = 0; at < n; at += k) {
        unsigned flags;
        k = n - at > h->max_frame ? h->max_frame : n - at;
        flags = (at + k == n ? FL_END_HEADERS : 0) | (!at && end_stream ? FL_END_STREAM : 0);
        if (frame(w, c, at ? T_CONT : T_HEADERS, flags, f->sid, b + at, k))
            bad = 1;
    }
    w->io.release(w->io.env, b);
    if (end_stream) {
        f->end_sent = 1;
        f->state = F_HEAD;
    }
    return bad ? -1 : 0;
}

/* DATA for a stream, as far as the windows let it. */
static int send_data(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    struct wfn_h2 *h = c->h2s;
    unsigned char b[4096];
    int moved = 0;
    while (!f->end_sent && c->out_len - c->out_at < 65536) {
        long allow = h->send_win < f->swin ? h->send_win : f->swin;
        unsigned long n;
        int last;
        if (!f->up_queued) {
            if (!wfn_up_done(f))
                break;
            frame(w, c, T_DATA, FL_END_STREAM, f->sid, b, 0);
            f->end_sent = 1;
            moved = 1;
            break;
        }
        if (allow <= 0)
            break;
        n = wfn_up_take(w, f, b, (unsigned long)allow < sizeof b ? (unsigned long)allow : sizeof b);
        last = wfn_up_done(f) && !f->up_queued;
        if (frame(w, c, T_DATA, last ? FL_END_STREAM : 0, f->sid, b, n))
            return -1;
        h->send_win -= (long)n;
        f->swin -= (long)n;
        moved = 1;
        if (last)
            f->end_sent = 1;
    }
    if (f->end_sent && f->state == F_SEND)
        f->state = F_HEAD;
    return moved;
}

/* ---- receiving ------------------------------------------------------------------ */

/* Whether the body fell short of its Content-Length. */
static int short_body(const struct wfn_fetch *f)
{
    return f->framing == FR_LENGTH && f->left && !f->discard;
}

/* The stream's answer is all in. */
static void stream_end(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f)
{
    f->end_recv = 1;
    if (short_body(f)) {
        wfn_fail(w, f, WFN_E_CLOSED, "the stream ended before its Content-Length");
        return;
    }
    wfn_h2_detach(w, c, f, !f->end_sent);
    wfn_body_end(w, f);
}

static int headers_done(struct wfn *w, struct wfn_conn *c, wu32 sid, int end_stream)
{
    struct wfn_h2 *h = c->h2s;
    struct wfn_fetch *f;
    char *text;
    long n;
    /* Decoded once (the table changes as it goes), into the most a head may be. */
    if (!(text = w->io.alloc(w->io.env, MAX_BLOCK)))
        return conn_error(w, c, E_PROTOCOL, "no memory");
    n = wfn_hpack_decode(h->dec, h->block, h->block_len, text, MAX_BLOCK);
    h->block_len = 0;
    if (n < 0) {
        w->io.release(w->io.env, text);
        return conn_error(w, c, E_COMPRESSION, "a bad header block");
    }
    f = stream(w, c, sid);
    if (f && !f->head_ready && !f->discard) {
        const char *v;
        unsigned long vl, status = 0, i, at = 0;
        char line[12];
        long cl;
        if ((v = wfn_hdr(text, (unsigned long)n, ":status", &vl, NULL)) != NULL && vl == 3)
            for (i = 0; i < 3; i++)
                status = status * 10 + (unsigned long)(v[i] - '0');
        if (status < 100 || status > 999) {
            w->io.release(w->io.env, text);
            rst(w, c, sid, E_PROTOCOL);
            f->end_recv = f->end_sent = 1;
            wfn_fail(w, f, WFN_E_PROTOCOL, "an HTTP/2 answer without :status");
            return 1;
        }
        if (status >= 200) {
            /* the lines without the pseudo-headers */
            const char *p = text, *end = text + n;
            char *q = text;
            while (p < end) {
                const char *nl = p;
                while (nl < end && *nl != '\n')
                    nl++;
                if (*p != ':') {
                    memmove(q, p, (unsigned long)(nl - p) + (nl < end));
                    q += (nl - p) + (nl < end);
                }
                p = nl + 1;
            }
            memcpy(line, "HTTP/2 ", 7);
            line[7] = (char)('0' + status / 100), line[8] = (char)('0' + status / 10 % 10), line[9] = (char)('0' + status % 10);
            if ((v = wfn_hdr(text, (unsigned long)(q - text), "content-length", &vl, NULL)) != NULL) {
                for (cl = 0, i = 0; i < vl && v[i] >= '0' && v[i] <= '9' && cl < 0x7ffffff; i++)
                    cl = cl * 10 + (v[i] - '0');
                f->length = (unsigned long)cl;
                f->hflags |= WFN_H_LENGTH;
                if (vl && i == vl)                  /* the DATA is checked against it */
                    f->framing = FR_LENGTH, f->left = f->length;
            }
            if (status == 204 || status == 304 || wfn_ieq(f->method, strlen(f->method), "HEAD")) {
                f->hflags |= WFN_H_NOBODY;
                f->length = 0;
                f->framing = FR_NONE;
            }
            at = (unsigned long)wfn_set_head(w, f, status, 20, line, 10, text, (unsigned long)(q - text));
            if ((long)at < 0) {
                w->io.release(w->io.env, text);
                wfn_fail(w, f, WFN_E_HOST, "no memory");
                return 1;
            }
            f->state = F_BODY;
        }
    }
    w->io.release(w->io.env, text);
    if (f && end_stream)
        stream_end(w, c, f);
    return 1;
}

static int handle(struct wfn *w, struct wfn_conn *c, unsigned type, unsigned flags, wu32 sid, const unsigned char *p,
                  unsigned long len)
{
    struct wfn_h2 *h = c->h2s;
    struct wfn_fetch *f;
    unsigned long pad = 0, i;
    if (h->cont_sid && (type != T_CONT || sid != h->cont_sid))
        return conn_error(w, c, E_PROTOCOL, "a header block was cut");
    if ((type == T_DATA || type == T_HEADERS) && (flags & FL_PADDED)) {
        if (!len || p[0] >= len)
            return conn_error(w, c, E_PROTOCOL, "bad padding");
        pad = p[0];
    }
    switch (type) {
    case T_DATA:
        if (!sid)
            return conn_error(w, c, E_PROTOCOL, "DATA on stream 0");
        h->credit += len;
        if (h->credit >= h->conn_win / 2) {
            window_update(w, c, 0, h->credit);
            h->credit = 0;
        }
        f = stream(w, c, sid);
        if (!f || f->end_recv)
            return 1;
        if (!f->head_ready && !f->discard) {
            rst(w, c, sid, E_PROTOCOL);
            f->end_recv = f->end_sent = 1;
            wfn_fail(w, f, WFN_E_PROTOCOL, "DATA before the head");
            return 1;
        }
        {
            const unsigned char *d = p + ((flags & FL_PADDED) ? 1 : 0);
            unsigned long dl = len - pad - ((flags & FL_PADDED) ? 1 : 0), took;
            if (f->framing == FR_LENGTH && !f->discard) {
                if (dl > f->left) {
                    rst(w, c, sid, E_PROTOCOL);
                    f->end_recv = f->end_sent = 1;
                    wfn_fail(w, f, WFN_E_PROTOCOL, "more DATA than its Content-Length");
                    return 1;
                }
                f->left -= dl;
            }
            took = wfn_deliver(w, f, d, dl);
            if (f->body_done && f->error)
                return 1;                           /* the decoder failed */
            if (f->dec || f->discard)
                wfn_h2_consumed(w, c, f, took);
            wfn_h2_consumed(w, c, f, len - dl);     /* padding never reaches the ring */
            if (took < dl)
                wfn_stash(w, f, d + took, dl - took);
        }
        if (flags & FL_END_STREAM)
            stream_end(w, c, f);
        return 1;
    case T_HEADERS:
    case T_CONT:
        if (!sid)
            return conn_error(w, c, E_PROTOCOL, "HEADERS on stream 0");
        if (type == T_HEADERS) {
            unsigned long skip = ((flags & FL_PADDED) ? 1 : 0) + ((flags & FL_PRIORITY) ? 5 : 0);
            if (skip + pad > len)
                return conn_error(w, c, E_PROTOCOL, "a bad HEADERS frame");
            p += skip;
            len -= skip + pad;
            h->cont_end = (unsigned char)(flags & FL_END_STREAM);
        }
        if (h->block_len + len > MAX_BLOCK)
            return conn_error(w, c, E_PROTOCOL, "a header block over 64 KB");
        if (!h->block && !(h->block = w->io.alloc(w->io.env, MAX_BLOCK)))
            return conn_error(w, c, E_PROTOCOL, "no memory");
        memcpy(h->block + h->block_len, p, len);
        h->block_len += len;
        if (!(flags & FL_END_HEADERS)) {
            h->cont_sid = sid;
            return 1;
        }
        h->cont_sid = 0;
        return headers_done(w, c, sid, h->cont_end);
    case T_PRIORITY:
        return 1;
    case T_RST:
        if (len != 4 || !sid)
            return conn_error(w, c, E_FRAME_SIZE, "a bad RST_STREAM");
        if ((f = stream(w, c, sid)) != NULL) {
            wu32 code = get32(p);
            f->end_recv = f->end_sent = 1;
            if (code == E_REFUSED && !f->got_bytes && wfn_resendable(f) && f->retries < 2) {
                wfn_h2_detach(w, c, f, 0);
                f->retries++;
                wfn_requeue(w, f);
            } else if (code == E_NO_ERROR && f->head_ready && !short_body(f)) {
                wfn_h2_detach(w, c, f, 0);
                wfn_body_end(w, f);
            } else
                wfn_fail(w, f, WFN_E_CLOSED, "the server reset the stream");
        }
        return 1;
    case T_SETTINGS:
        if (sid || (flags & FL_ACK ? len != 0 : len % 6))
            return conn_error(w, c, E_FRAME_SIZE, "a bad SETTINGS frame");
        if (flags & FL_ACK)
            return 1;
        for (i = 0; i < len; i += 6) {
            unsigned id = (unsigned)p[i] << 8 | p[i + 1];
            wu32 v = get32(p + i + 2);
            if (id == 3)
                h->max_streams = v;
            else if (id == 4) {
                long delta = (long)v - (long)h->init_win;
                if (v > 0x7fffffffUL)
                    return conn_error(w, c, E_FLOW, "a window over 2 GB");
                for (f = w->fetches; f; f = f->next)
                    if (f->conn == c && f->sid)
                        f->swin += delta;
                h->init_win = v;
            } else if (id == 5) {
                if (v < 16384 || v > 16777215)
                    return conn_error(w, c, E_PROTOCOL, "a bad MAX_FRAME_SIZE");
                h->max_frame = v > WFN_FRAME ? WFN_FRAME : v;
            }
        }
        frame(w, c, T_SETTINGS, FL_ACK, 0, p, 0);
        return 1;
    case T_PUSH:
        return conn_error(w, c, E_PROTOCOL, "PUSH_PROMISE, with push turned off");
    case T_PING:
        if (len != 8 || sid)
            return conn_error(w, c, E_FRAME_SIZE, "a bad PING");
        if (!(flags & FL_ACK))
            frame(w, c, T_PING, FL_ACK, 0, p, 8);
        return 1;
    case T_GOAWAY:
        if (len < 8 || sid)
            return conn_error(w, c, E_FRAME_SIZE, "a bad GOAWAY");
        {
            wu32 last = get32(p) & 0x7fffffffUL;
            struct wfn_fetch *next;
            h->goaway = 1;
            for (f = w->fetches; f; f = next) {
                next = f->next;
                if (f->conn != c || (f->sid && f->sid <= last))
                    continue;
                if (!f->got_bytes && wfn_resendable(f) && f->retries < 2) {
                    f->end_recv = f->end_sent = 1;
                    wfn_h2_detach(w, c, f, 0);
                    f->retries++;
                    wfn_requeue(w, f);
                } else {
                    f->end_recv = f->end_sent = 1;
                    wfn_fail(w, f, WFN_E_CLOSED, "the server went away");
                }
            }
        }
        return 1;
    case T_WINDOW:
        if (len != 4)
            return conn_error(w, c, E_FRAME_SIZE, "a bad WINDOW_UPDATE");
        if (!sid)
            h->send_win += (long)(get32(p) & 0x7fffffffUL);
        else if ((f = stream(w, c, sid)) != NULL)
            f->swin += (long)(get32(p) & 0x7fffffffUL);
        return 1;
    }
    return 1;                                       /* other frame types are ignored */
}

int wfn_h2_step(struct wfn *w, struct wfn_conn *c)
{
    struct wfn_h2 *h = c->h2s;
    struct wfn_fetch *f;
    int moved = 0;
    long r;
    /* New streams, and DATA for those uploading. */
    for (f = w->fetches; f; f = f->next) {
        if (f->conn != c)
            continue;
        if (f->state == F_SEND && !f->sid) {
            if (h->goaway || (unsigned long)h->open >= h->max_streams)
                continue;
            if (send_headers(w, c, f)) {
                wfn_fail(w, f, WFN_E_HOST, "no memory");
                continue;
            }
            moved = 1;
        }
        if (f->sid && !f->end_sent) {
            int m = send_data(w, c, f);
            if (m < 0) {
                wfn_fail(w, f, WFN_E_HOST, "no memory");
                continue;
            }
            moved |= m;
        }
    }
    if (wfn_flush(w, c)) {
        wfn_conn_fail(w, c, WFN_E_CLOSED, "the connection closed");
        return 1;
    }
    /* Frames from the server. */
    for (;;) {
        r = wfn_conn_read(w, c);
        if (r == 0 || r == WFN_FAIL) {
            wfn_conn_fail(w, c, WFN_E_CLOSED, "the connection closed");
            return 1;
        }
        while (c->in_len - c->in_at >= 9) {
            const unsigned char *p = c->in + c->in_at;
            unsigned long len = (unsigned long)p[0] << 16 | (unsigned long)p[1] << 8 | p[2];
            if (len > WFN_FRAME)
                return conn_error(w, c, E_FRAME_SIZE, "a frame over 16 KB");
            if (c->in_len - c->in_at < 9 + len)
                break;
            c->in_at += 9 + len;
            handle(w, c, p[3], p[4], get32(p + 5) & 0x7fffffffUL, p + 9, len);
            moved = 1;
            if (c->state == C_DEAD)
                return 1;
        }
        if (r <= 0)
            break;
    }
    if (wfn_flush(w, c)) {
        wfn_conn_fail(w, c, WFN_E_CLOSED, "the connection closed");
        return 1;
    }
    if (h->goaway && !h->attached)
        c->state = C_DEAD;
    return moved;
}
