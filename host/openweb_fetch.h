/*
 * openweb.fetch/1: HTTP and HTTPS fetches for an Amiga, done here with
 * OpenSSL 3, zlib and Brotli (docs/OPENWEB_FETCH.md). Each fetch is a handle with
 * a thread of its own; the calls only start, ask, read and stop it, so
 * many run at once. The emulator's service host calls this.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OPENWEB_FETCH_H
#define OPENWEB_FETCH_H

#include <stdint.h>

#define WF_OPEN   1   /* buf0 the request text, buf1 the body (or its start); extra[0] flags, extra[1] body length, extra[2] timeout s -> result handle */
#define WF_SEND   2   /* arg handle, buf0 body bytes; extra[0] WF_LAST, extra[1] wait ms -> result bytes taken */
#define WF_HEAD   3   /* arg handle, buf0 (out) the head; extra[0] wait ms -> result HTTP status, aux error (0 and 0: not yet) */
#define WF_READ   4   /* arg handle, buf0 (out) body bytes; extra[0] wait ms -> result bytes, aux WF_END | error << 8 */
#define WF_CANCEL 5   /* arg handle */
#define WF_CLOSE  6   /* arg handle */
#define WF_STATS  7   /* -> result fetches open, aux threads running */

/* OPEN: extra[0] */
#define WF_BODY   1u          /* a body follows: buf1, then SEND */
#define WF_DECODE 2u          /* undo gzip and deflate here */
/* OPEN: extra[1] */
#define WF_LENGTH_UNKNOWN 0xffffffffu
/* SEND: extra[0] */
#define WF_LAST   1u
/* READ: aux */
#define WF_MORE   0u
#define WF_END    1u          /* no more after these bytes; bits 8-15 the error when it failed */

/* The head block: 32 bytes of big-endian u32s, the header text, the info text. */
#define WF_HEAD_MAGIC 0x4f574631u   /* 'OWF1' */
#define WF_HEAD_SIZE  32u
#define WF_H_TLS      1u
#define WF_H_VERIFIED 2u
#define WF_H_DECODED  4u
#define WF_H_LENGTH   8u
#define WF_H_NOBODY   16u

/* Errors: the head's error field and READ's aux bits 8-15. */
#define WF_E_DNS       1
#define WF_E_CONNECT   2
#define WF_E_TLS       3
#define WF_E_TIMEOUT   4
#define WF_E_PROTOCOL  5
#define WF_E_CANCELLED 6
#define WF_E_CLOSED    7
#define WF_E_DECODE    8
#define WF_E_HOST      9

/* Status, as the services card's. */
#define WF_OK          0
#define WF_BADREQUEST (-2)
#define WF_TOOSMALL   (-4)
#define WF_HOSTERROR  (-5)

struct wf_buffer {
    const uint8_t *in;     /* the Amiga's bytes, for a buffer the service reads */
    uint8_t *out;          /* where to write, for a buffer the service writes */
    uint32_t length;       /* in: its length; out: the room */
    uint32_t written;      /* out: bytes written */
};

/* Runs one request. Returns the status; sets *result and *aux. */
int wf_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct wf_buffer buf[4],
            uint32_t *result, uint32_t *aux);

/* Answering later, for the services card (AmiCradle's services.c): wf_start
 * is wf_call with the card's id for the request and its completion. It
 * answers now (returns the status, sets *result and *aux), or returns
 * WF_LATER: a HEAD or READ with a wait and nothing to give yet is held, and
 * done(id, status, result, aux) is called once, from one of this library's
 * threads, when the head or body bytes come, the fetch ends or is
 * cancelled, or the wait runs out. Until then buf[0].out is the library's to
 * write; nothing else of buf[] is kept. wf_cancel(id): the card no longer
 * wants request id; if it is held, done is called at once with
 * WF_CALL_CANCELLED. Other ops, and waits of 0, run as wf_call does. */
#define WF_LATER           1
#define WF_CALL_CANCELLED (-3)
typedef void (*wf_done_fn)(uint32_t id, int32_t status, uint32_t result, uint32_t aux);
int wf_start(uint16_t op, uint32_t arg, const uint32_t extra[4], struct wf_buffer buf[4],
             uint32_t *result, uint32_t *aux, uint32_t id, wf_done_fn done);
void wf_cancel(uint32_t id);

#endif
