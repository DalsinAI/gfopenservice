/*
 * wfnet: openweb.fetch done on the machine itself (docs/OPENWEB_FETCH.md,
 * "The native transport"). The same ops, buffers and answers as the
 * service, served by this code over the machine's own sockets and TLS:
 * HTTP/1.1 with kept connections, HTTP/2 over TLS (ALPN "h2") with HPACK,
 * WebSocket, HTTP proxies and CONNECT tunnels, Basic and Digest auth.
 *
 * Portable C (C99, nothing of the C library but <string.h>),
 * with no writable globals: everything lives in the struct wfn that
 * wfn_new() makes. The machine plugs in through struct wfn_io: sockets,
 * TLS, time, memory, random bytes and the environment. src/wfnet_amiga.c
 * is bsdsocket.library and opentls.library; host/wfnet_posix.c is POSIX
 * sockets and OpenSSL, for the tests.
 *
 * Nothing runs in the background: every call moves every fetch on, for at
 * most its wait, so one task can have many fetches in flight on one
 * struct wfn. A struct wfn belongs to one task.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENWEB_WFNET_H
#define OPENWEB_WFNET_H

/* Ops: as openweb.fetch/1 (OPEN to STATS), and the WebSocket ops of v2. */
#define WFN_OPEN    1
#define WFN_SEND    2
#define WFN_HEAD    3
#define WFN_READ    4
#define WFN_CANCEL  5
#define WFN_CLOSE   6
#define WFN_STATS   7
#define WFN_WS_OPEN 8
#define WFN_WS_SEND 9
#define WFN_WS_RECV 10

/* OPEN flags (extra[0]) */
#define WFN_F_BODY    1UL       /* a body follows */
#define WFN_F_DECODE  2UL       /* undo Content-Encoding (through the decoder hook) */
#define WFN_F_NOPROXY 4UL       /* never through a proxy */
#define WFN_F_HTTP1   8UL       /* HTTP/1.1 only: no ALPN "h2" */
#define WFN_LENGTH_UNKNOWN 0xffffffffUL

/* WS_SEND extra[0]: the opcode (bits 0-3); bit 7: more parts of this
 * message follow. WS_RECV aux: the opcode (bits 0-3), WFN_WS_FIN when the
 * message ends with these bytes, WFN_WS_END when the connection has ended
 * (the error in bits 8-15), and for a close the code in bits 16-31. */
#define WFN_WS_CONT   0
#define WFN_WS_TEXT   1
#define WFN_WS_BINARY 2
#define WFN_WS_CLOSE  8
#define WFN_WS_PING   9
#define WFN_WS_PONG   10
#define WFN_WS_MORE   0x80UL
#define WFN_WS_FIN    0x10UL
#define WFN_WS_END    0x20UL

/* Statuses, as the services card's. */
#define WFN_OK          0
#define WFN_BADREQUEST (-2)
#define WFN_TOOSMALL   (-4)
#define WFN_BUSY       (-5)

/* Errors (the head's error, READ's aux bits 8-15), as openweb.fetch/1. */
#define WFN_E_DNS       1
#define WFN_E_CONNECT   2
#define WFN_E_TLS       3
#define WFN_E_TIMEOUT   4
#define WFN_E_PROTOCOL  5
#define WFN_E_CANCELLED 6
#define WFN_E_CLOSED    7
#define WFN_E_DECODE    8
#define WFN_E_HOST      9

/* The head's flags. */
#define WFN_H_TLS      1UL
#define WFN_H_VERIFIED 2UL
#define WFN_H_DECODED  4UL
#define WFN_H_LENGTH   8UL
#define WFN_H_NOBODY   16UL

/* What the seam's socket and TLS calls answer besides a count. */
#define WFN_AGAIN      (-1)     /* would block: wait until readable */
#define WFN_AGAIN_W    (-2)     /* would block: wait until writable */
#define WFN_FAIL       (-3)
#define WFN_BROKEN     (-4)     /* wait: the task was told to stop (Ctrl-C) */

#define WFN_WANT_READ  1
#define WFN_WANT_WRITE 2

/* One buffer of a call, as the service's: in for bytes it reads, out
 * (with length the room) for bytes it writes, written set by it. */
struct wfn_buf {
    const unsigned char *in;
    unsigned char *out;
    unsigned long length;
    unsigned long written;
};

/* The machine. env is handed back to every call. */
struct wfn_io {
    void *env;
    void *(*alloc)(void *env, unsigned long size);          /* zeroed, or NULL */
    void (*release)(void *env, void *p);
    unsigned long (*now)(void *env);                        /* milliseconds; may wrap */
    /* Resolves host and starts a non-blocking connect: the socket, or -1
     * with *error WFN_E_DNS or WFN_E_CONNECT. */
    long (*connect)(void *env, const char *host, unsigned port, int *error);
    int (*connected)(void *env, long sock);                 /* once writable: 0, or WFN_E_CONNECT */
    long (*send)(void *env, long sock, const void *p, unsigned long n);  /* count, WFN_AGAIN_W or WFN_FAIL */
    long (*recv)(void *env, long sock, void *p, unsigned long n);        /* count, 0 the end, WFN_AGAIN or WFN_FAIL */
    void (*close)(void *env, long sock);
    /* Waits at most ms for one of n sockets to be ready as want[i] asks
     * (WFN_WANT_*); ready[i] says which. 0, or WFN_BROKEN. */
    int (*wait)(void *env, const long *socks, const unsigned char *want, unsigned char *ready, int n,
                unsigned long ms);
    /* TLS on a connected socket: alpn is "h2,http/1.1" or "http/1.1". */
    void *(*tls_new)(void *env, long sock, const char *host, const char *alpn);
    int (*tls_handshake)(void *env, void *tls);             /* 0, WFN_AGAIN, WFN_AGAIN_W or WFN_FAIL */
    long (*tls_read)(void *env, void *tls, void *p, unsigned long n);    /* as recv */
    long (*tls_write)(void *env, void *tls, const void *p, unsigned long n);  /* as send */
    long (*tls_pending)(void *env, void *tls);              /* bytes readable without the socket */
    /* The info lines ("tls=...\ncipher=...\n", or after a failure
     * "error=...\n" and the certificate's summary) into out: their length.
     * *verified: the certificate was checked and trusted. */
    unsigned long (*tls_info)(void *env, void *tls, char *out, unsigned long room, int *verified);
    const char *(*tls_alpn)(void *env, void *tls);          /* what the server chose, or "" */
    void (*tls_free)(void *env, void *tls);
    void (*random)(void *env, void *p, unsigned long n);
    /* An environment variable ("http_proxy") into out, NUL ended: its
     * length, or -1 when it is not set. */
    long (*getenv)(void *env, const char *name, char *out, unsigned long room);
    /* Content decoding for WFN_F_DECODE: stream.datatype's gzip and brotli
     * classes plug in here (src/wfnet_dec.c). dec_new answers NULL for a
     * coding it cannot undo; then, and when dec_new itself is NULL, the
     * body comes as the server sent it, with Content-Encoding kept and
     * "decode=datatypes" in the info text. dec_run takes in (setting
     * *used) and writes out: bytes written, or < 0 when the data is bad.
     * last: no more input. */
    void *(*dec_new)(void *env, const char *coding);
    long (*dec_run)(void *env, void *dec, const unsigned char *in, unsigned long length, unsigned long *used,
                    unsigned char *out, unsigned long room, int last);
    void (*dec_free)(void *env, void *dec);
    /* Accept-Encoding with WFN_F_DECODE: the codings dec_new can undo now
     * ("gzip, deflate, br"); NULL, or no dec_accept, sends "identity". */
    const char *(*dec_accept)(void *env);
    /* What undid a body, for the info text ("decode=gzip.datatype"). */
    const char *(*dec_name)(void *env, void *dec);
};

struct wfn;

/* window: body bytes read ahead for each fetch (0: 64 KB, at least 16 KB). */
struct wfn *wfn_new(const struct wfn_io *io, unsigned long window);
/* Closes every fetch and connection. */
void wfn_free(struct wfn *w);
/* One op, as the service answers it: the status; *result and *aux set. */
long wfn_call(struct wfn *w, unsigned op, unsigned long arg, const unsigned long extra[4], struct wfn_buf buf[4],
              unsigned long *result, unsigned long *aux);

/* Without a call to make: moves every fetch on, waits at most ms (2000 at
 * most) for one of the sockets (or the machine's wait to be broken, as by
 * a signal), and moves them on again. A task running many fetches pumps,
 * then asks each one with a wait of 0, and never blocks on any one. */
void wfn_pump(struct wfn *w, unsigned long ms);

/* Pieces with their own tests (RFC 7541 Appendix C, RFC 1321, FIPS 180-4,
 * RFC 4648, RFC 6455 and RFC 7616 examples). */
struct wfn_hpack;
unsigned long wfn_huff_encode(const unsigned char *in, unsigned long n, unsigned char *out, unsigned long room);
long wfn_huff_decode(const unsigned char *in, unsigned long n, unsigned char *out, unsigned long room);
struct wfn_hpack *wfn_hpack_new(const struct wfn_io *io, unsigned long table);
void wfn_hpack_free(const struct wfn_io *io, struct wfn_hpack *h);
/* A header block into "name: value\n" lines: their length, or -1. */
long wfn_hpack_decode(struct wfn_hpack *h, const unsigned char *in, unsigned long n, char *out, unsigned long room);
/* One header into the block at out (names lower case): bytes written, or 0. index: add it to the table. */
unsigned long wfn_hpack_encode(struct wfn_hpack *h, const char *name, unsigned long nl, const char *value,
                               unsigned long vl, int index, int huffman, unsigned char *out, unsigned long room);
unsigned long wfn_hpack_table_size(const struct wfn_hpack *h);

void wfn_md5(const void *p, unsigned long n, unsigned char out[16]);
void wfn_sha1(const void *p, unsigned long n, unsigned char out[20]);
void wfn_sha256(const void *p, unsigned long n, unsigned char out[32]);
unsigned long wfn_base64(const unsigned char *in, unsigned long n, char *out);
/* Sec-WebSocket-Accept for a key: 28 characters and a NUL. */
void wfn_ws_accept(const char *key, char out[29]);
/* The Authorization value for a challenge (the header's value): its
 * length, or 0 when the challenge is not one this can answer. */
unsigned long wfn_auth_answer(const char *challenge, unsigned long cl, const char *user, const char *password,
                              const char *method, const char *uri, unsigned long nc, const char *cnonce,
                              char *out, unsigned long room);

#endif
