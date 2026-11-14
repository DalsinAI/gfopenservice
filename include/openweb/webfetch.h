/*
 * webfetch: the Amiga's side of openweb.fetch/1 (docs/OPENWEB_FETCH.md), for
 * any program to link: OWFetch now, GFOpenBrowser later.
 *
 *   struct wf_service s;
 *   wf_service_open(&s);                     -> 0, or OSERR_NOSERVICE
 *   wf_open(&s, "GET https://example.com/\n", 0, WF_DECODE, 0, 0, NULL, 0, &fetch);
 *   wf_head(&s, fetch, buf, room, 500, &head) until it returns 0
 *   wf_read(&s, fetch, buf, room, 1000, &got, &end) until end
 *   wf_close(&s, fetch);
 *   wf_service_close(&s);
 *
 * Each call is one request through gfopenservice.device, which takes it to
 * the services card or a paired Cradle; or, when neither offers the
 * service (or WF_MODE_NATIVE asks for it), is answered here by the native
 * transport (src/wfnet*.c) over bsdsocket.library and gfopentls.library,
 * with the same messages. The waits are in milliseconds, at most 2000: a
 * call answers sooner when something happens.
 *
 * Many fetches from one task without blocking (docs/OPENWEB_FETCH.md, "One
 * task, many fetches"): start each HEAD, READ or WS_RECV with
 * wf_async_start(), sleep in wf_wait() on the service and the task's own
 * signals, and collect what is answered with wf_async_check(). On the card
 * each is a SendIO() answered later (the card holds it, wf_start); on the
 * native transport wf_wait() moves every fetch on.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OPENWEB_WEBFETCH_H
#define OPENWEB_WEBFETCH_H

#include <exec/types.h>
#include "devices/mitchy.h"

#define WF_SERVICE_NAME "openweb.fetch/1"

#define WF_PROTOCOL_VERSION 2      /* docs/OPENWEB_FETCH.md: v2 adds the WebSocket ops and the native transport */

/* Ops */
#define WF_OP_OPEN   1
#define WF_OP_SEND   2
#define WF_OP_HEAD   3
#define WF_OP_READ   4
#define WF_OP_CANCEL 5
#define WF_OP_CLOSE  6
#define WF_OP_STATS  7
#define WF_OP_WS_OPEN 8
#define WF_OP_WS_SEND 9
#define WF_OP_WS_RECV 10

/* wf_open flags */
#define WF_BODY   1UL           /* a body follows: body, then wf_send */
#define WF_DECODE 2UL           /* gzip, deflate and br undone (native: through the datatypes' hook) */
#define WF_NOPROXY 4UL          /* never through a proxy (native) */
#define WF_HTTP1  8UL           /* HTTP/1.1 only: no ALPN "h2" (native) */
#define WF_LENGTH_UNKNOWN 0xffffffffUL

/* wf_head: the head's flags */
#define WF_H_TLS      1UL
#define WF_H_VERIFIED 2UL
#define WF_H_DECODED  4UL
#define WF_H_LENGTH   8UL
#define WF_H_NOBODY   16UL

/* Errors: struct wf_head's error, and wf_read's *end >> 8 */
#define WF_E_DNS       1
#define WF_E_CONNECT   2
#define WF_E_TLS       3
#define WF_E_TIMEOUT   4
#define WF_E_PROTOCOL  5
#define WF_E_CANCELLED 6
#define WF_E_CLOSED    7
#define WF_E_DECODE    8
#define WF_E_HOST      9

/* wf_head's answer when the head has not come yet. */
#define WF_NOTYET 1

/* WebSocket: wf_ws_send's op (| WF_WS_MORE for a part of a message), and
 * wf_ws_recv's *flags: the op, WF_WS_FIN at a message's end, WF_WS_END
 * when the connection has ended (the error in bits 8-15), and for a close
 * its code in bits 16-31. */
#define WF_WS_TEXT   1
#define WF_WS_BINARY 2
#define WF_WS_CLOSE  8
#define WF_WS_PING   9
#define WF_WS_PONG   10
#define WF_WS_MORE   0x80UL
#define WF_WS_FIN    0x10UL
#define WF_WS_END    0x20UL

/* wf_service_open_mode */
#define WF_MODE_AUTO    0       /* the card or a Cradle; else native (ENV:GFOpenWeb/Fetch "native" forces native) */
#define WF_MODE_NATIVE  1       /* this Amiga's own TCP/IP and GFOpenTLS only */
#define WF_MODE_SERVICE 2       /* the card or a Cradle only */
#define WF_WHERE_NATIVE 3       /* struct wf_service's where, beside OSWHERE_CARD and OSWHERE_LAN */

struct wfna;

struct wf_service {
    struct MsgPort *port;
    struct OSRequest *io;
    UWORD handle;               /* the service's, from OSCMD_OPEN */
    UBYTE open;
    UBYTE where;                /* OSWHERE_CARD, OSWHERE_LAN or WF_WHERE_NATIVE */
    struct wfna *native;        /* the native transport, when where is WF_WHERE_NATIVE */
    ULONG result, aux;          /* the last call's */
};

/* A call answered later (wf_async_start). */
struct wf_async {
    struct wf_service *s;
    struct OSRequest *io;       /* the card's: its own request, answered to the service's port */
    UWORD op;
    UBYTE busy, done;
    ULONG fetch, wait;
    void *buf;
    ULONG room;
    LONG status;                /* when done: as the plain call would answer */
    ULONG result, aux;
};

struct wf_head {
    ULONG error;                /* 0, or WF_E_*: then status is 0 */
    ULONG status;               /* the HTTP status */
    ULONG flags;                /* WF_H_* */
    ULONG length;               /* the body's length, with WF_H_LENGTH */
    ULONG version;              /* 10 or 11 */
    char *text;                 /* in the caller's buffer: the status line and the headers, each line ending in LF */
    ULONG text_length;
    char *info;                 /* key=value lines: the TLS summary, or error= */
    ULONG info_length;
    ULONG need;                 /* with OSERR_TOOSMALL: the room the head needs */
};

/* Opens openweb.fetch/1: 0, or OSERR_NOSERVICE (or another OSERR_*).
 * wf_service_open is WF_MODE_AUTO. */
LONG wf_service_open(struct wf_service *s);
LONG wf_service_open_mode(struct wf_service *s, ULONG mode);
void wf_service_close(struct wf_service *s);

/* Starts a fetch. request: "METHOD URL\n" then "Name: value\n" lines
 * (wf_request makes one); length 0 means NUL ended. With WF_BODY, body_length
 * is the body's whole length (or WF_LENGTH_UNKNOWN) and body/part its first
 * part (may be 0). timeout in seconds, 0 for 30. 0 and *fetch, or OSERR_*. */
LONG wf_open(struct wf_service *s, const char *request, ULONG length, ULONG flags, ULONG body_length,
             ULONG timeout, const void *body, ULONG part, ULONG *fetch);
/* The next part of the body (last: the final part). *taken is 0 when the
 * queue was full: send the same part again. */
LONG wf_send(struct wf_service *s, ULONG fetch, const void *data, ULONG length, BOOL last, ULONG wait, ULONG *taken);
/* The head into buf, parsed into *h: 0 when it is there (or the fetch
 * failed: h->error), WF_NOTYET, OSERR_TOOSMALL (h->need), or OSERR_*. */
LONG wf_head(struct wf_service *s, ULONG fetch, UBYTE *buf, ULONG room, ULONG wait, struct wf_head *h);
/* Body bytes into buf: *got of them; *end 0 while more is to come, else 1 |
 * the error << 8 (0 when the body ended well). */
LONG wf_read(struct wf_service *s, ULONG fetch, void *buf, ULONG room, ULONG wait, ULONG *got, ULONG *end);
LONG wf_cancel(struct wf_service *s, ULONG fetch);
LONG wf_close(struct wf_service *s, ULONG fetch);

/* WebSocket (ws:// or wss://): as wf_open; then wf_head gives the 101. */
LONG wf_ws_open(struct wf_service *s, const char *request, ULONG length, ULONG flags, ULONG timeout, ULONG *fetch);
/* One message, or a part of one (op | WF_WS_MORE): *taken 0 when the queue was full. */
LONG wf_ws_send(struct wf_service *s, ULONG fetch, const void *data, ULONG length, ULONG op, ULONG wait, ULONG *taken);
/* The next message's bytes: *got of them; *flags 0 when nothing came yet, else as above. */
LONG wf_ws_recv(struct wf_service *s, ULONG fetch, void *buf, ULONG room, ULONG wait, ULONG *got, ULONG *flags);

/* Asynchronous HEAD, READ and WS_RECV (op WF_OP_*): started now, answered
 * later. wf_async_check is TRUE once it is answered: a->status, a->result
 * and a->aux as the plain call answers (for HEAD, parse buf with
 * wf_head_parse). wf_async_abort takes one back unanswered. */
LONG wf_async_start(struct wf_service *s, struct wf_async *a, UWORD op, ULONG fetch, void *buf, ULONG room, ULONG wait);
BOOL wf_async_check(struct wf_async *a);
void wf_async_abort(struct wf_async *a);
/* Sleeps until something may have been answered, at most ms (native) or
 * until an answer comes (card: each held call comes back within its wait),
 * or one of sigs: which of sigs came. */
ULONG wf_wait(struct wf_service *s, ULONG ms, ULONG sigs);
/* A HEAD's answer in buf into *h (as wf_head): 0, or OSERR_BADREQUEST. */
LONG wf_head_parse(UBYTE *buf, ULONG room, struct wf_head *h);

/* The request text from its parts, NUL ended, into out: its length, or 0
 * when it does not fit. headers: count strings "Name: value". */
ULONG wf_request(char *out, ULONG room, const char *method, const char *url, const char *const *headers, LONG count);
/* A header's value from the head's text (the name in any case), NUL ended
 * into out; FALSE when the head has none. */
BOOL wf_header(const struct wf_head *h, const char *name, char *out, ULONG room);
/* An error's name, "DNS", "TLS", ... */
const char *wf_error_name(ULONG error);

#endif
