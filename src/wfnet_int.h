/*
 * wfnet's own parts, shared by src/wfnet.c (fetches, connections,
 * HTTP/1.1, proxies, auth, WebSocket) and src/wfnet_h2.c (HTTP/2).
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#ifndef WFNET_INT_H
#define WFNET_INT_H

typedef unsigned int wu32;      /* 32 bits on the 68k and on Linux */

#define WFN_MAX_FETCHES 64
#define WFN_MAX_HEAD    (64UL << 10)        /* a response head; the request text */
#define WFN_FRAME       16384UL             /* HTTP/2's frame size, ours and the default */
#define WFN_IN_ROOM     (WFN_FRAME + 9 + 1015)
#define WFN_UPLOAD_MAX  (64UL << 10)        /* body bytes queued to send */
#define WFN_OUT_MAX     (256UL << 10)       /* WebSocket frames queued to send */
#define WFN_INFO_ROOM   1024
#define WFN_HOST_ROOM   256
#define WFN_CRED_ROOM   128

/* struct wfn_conn state */
enum { C_CONNECT, C_TUNNEL, C_TLS, C_READY, C_DEAD };
/* struct wfn_fetch state */
enum { F_WAIT, F_CONNECTING, F_SEND, F_HEAD, F_BODY, F_DONE, F_WS };
/* how an HTTP/1.1 body is framed */
enum { FR_NONE, FR_LENGTH, FR_CHUNKED, FR_CLOSE };

struct wfn_ring {
    unsigned char *p;
    unsigned long size, at, len;
};

struct wfn_piece {
    struct wfn_piece *next;
    unsigned long length;
    unsigned char data[1];
};

struct wfn_h2;

struct wfn_conn {
    struct wfn_conn *next;
    long sock;
    void *tls;
    unsigned char state, use_tls, h2, ws, verified, may_h2, want, blocked_w, reused, forward;
    char key[2 * WFN_HOST_ROOM + 40];    /* where it goes: scheme, host, port, the proxy */
    char host[WFN_HOST_ROOM];           /* the origin: TLS's name */
    char tls_info[WFN_INFO_ROOM / 2];
    unsigned long tls_info_len;
    unsigned char *out;                 /* bytes to send: out[out_at..out_len) */
    unsigned long out_at, out_len, out_room;
    unsigned long in_at, in_len;        /* bytes read, not yet used: in[in_at..in_len) */
    unsigned long idle_at, idle_ms;     /* kept for later: since when, and for how long */
    struct wfn_fetch *owner;            /* the fetch on it (HTTP/1.1, WebSocket, a tunnel being made) */
    struct wfn_h2 *h2s;
    unsigned char in[WFN_IN_ROOM];
};

struct wfn_fetch {
    struct wfn_fetch *next;
    unsigned long handle;
    struct wfn_conn *conn;
    unsigned char state, use_tls, ws, tunnel, connect_phase, forward, discard, got_bytes;
    unsigned char head_ready, body_done, keep, reused, retries, auth_tried, pauth_tried, has_ua;
    unsigned char up_last, keep_body, end_sent, end_recv, sent_request, ws_close_sent, ws_more_out, no_decoder;
    unsigned long flags, timeout_ms, deadline, touched;
    char method[24];
    char host[WFN_HOST_ROOM];
    unsigned port;
    char phost[WFN_HOST_ROOM];          /* the proxy, when there is one */
    unsigned pport;
    char user[WFN_CRED_ROOM], pass[WFN_CRED_ROOM], puser[WFN_CRED_ROOM], ppass[WFN_CRED_ROOM];
    char *target;                       /* "/path?q" */
    char *headers;                      /* the caller's header lines, each ending CRLF */
    unsigned long headers_len;
    char *auth, *pauth;                 /* Authorization and Proxy-Authorization to send */
    /* the body to send */
    struct wfn_piece *up_head, *up_tail, *up_cur;
    unsigned long up_cur_at, up_queued, up_given, up_sent, body_length;
    /* the head */
    char *hbuf;                         /* HTTP/1.1: the head as it comes */
    unsigned long hbuf_len;
    unsigned long status, hflags, version, length, error;
    char *htext;
    unsigned long hlen;
    char info[WFN_INFO_ROOM];
    unsigned long info_len;
    /* HTTP/1.1 body framing */
    int framing, chunk_state;
    unsigned long left;
    /* the body read ahead, and (when decoding) the coded bytes not yet decoded */
    struct wfn_ring ring, raw;
    void *dec;
    /* HTTP/2 */
    wu32 sid;
    long swin;                          /* what the server lets us send on the stream */
    unsigned long rcredit;              /* bytes read by the caller, not yet given back */
    /* WebSocket */
    char ws_key[32];
    unsigned long ws_left, rec_left;
    unsigned ws_op, ws_msg_op, ws_fin, rec_flags, close_code;
};

struct wfn {
    struct wfn_io io;
    unsigned long window;
    struct wfn_fetch *fetches;
    struct wfn_conn *conns;
    unsigned long next_handle;
    int nfetch, nconn;
};

/* src/wfnet.c, for the HTTP/2 side */
int wfn_out(struct wfn *w, struct wfn_conn *c, const void *p, unsigned long n);
unsigned long wfn_deliver(struct wfn *w, struct wfn_fetch *f, const unsigned char *p, unsigned long n);
void wfn_stash(struct wfn *w, struct wfn_fetch *f, const unsigned char *p, unsigned long n);
void wfn_body_end(struct wfn *w, struct wfn_fetch *f);
void wfn_fail(struct wfn *w, struct wfn_fetch *f, unsigned long error, const char *why);
int wfn_set_head(struct wfn *w, struct wfn_fetch *f, unsigned long status, unsigned long version, const char *sline,
                 unsigned long sl, const char *lines, unsigned long len);
const char *wfn_hdr(const char *text, unsigned long len, const char *name, unsigned long *vl, const char **from);
int wfn_ieq(const char *a, unsigned long n, const char *b);
void wfn_requeue(struct wfn *w, struct wfn_fetch *f);
int wfn_resendable(const struct wfn_fetch *f);
long wfn_conn_read(struct wfn *w, struct wfn_conn *c);
void wfn_conn_fail(struct wfn *w, struct wfn_conn *c, unsigned long error, const char *why);
unsigned long wfn_up_take(struct wfn *w, struct wfn_fetch *f, unsigned char *out, unsigned long room);
int wfn_flush(struct wfn *w, struct wfn_conn *c);
int wfn_up_done(const struct wfn_fetch *f);
void wfn_info(struct wfn_fetch *f, const char *key, const char *value);
const char *wfn_accept(struct wfn *w);

/* src/wfnet_h2.c */
int wfn_h2_start(struct wfn *w, struct wfn_conn *c);
int wfn_h2_step(struct wfn *w, struct wfn_conn *c);
void wfn_h2_free(struct wfn *w, struct wfn_conn *c);
int wfn_h2_room(const struct wfn_conn *c);
void wfn_h2_attach(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f);
void wfn_h2_detach(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f, int cancel);
void wfn_h2_consumed(struct wfn *w, struct wfn_conn *c, struct wfn_fetch *f, unsigned long n);
int wfn_h2_busy(const struct wfn_conn *c);

#endif
