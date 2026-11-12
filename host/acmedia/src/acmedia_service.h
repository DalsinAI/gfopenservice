/*
 * Copyright (c) 2026 AmiCradle Team. MIT.
 *
 * Copied from gfosmotherboards amiga-native-bodies/native/acmedia_service.h
 * (the runtime's side of libacmedia.so's interface); keep the two the same.
 *
 * ACMedia's broker in the runtime (designs/ACMEDIA-DESIGN-20261110.md 2,
 * M4K step 4): the twelve host services ACM_CAPS ... ACM_FAULT
 * ($00012000-$0001200B, hostlib/acmedia/acmedia.h; HOSTLIB-ABI.md 8.7) as
 * AC090 runs them, in front of a backend that does the decoding.
 *
 * This side owns what the ABI promises the guest, whatever the backend:
 *   - the gates: a block or read range that is not plain memory, or a
 *     runtime with no backend, leaves the call to its 68k body, which
 *     answers ACM_E_NOTAVAIL (so akikoplus.device takes its next rung);
 *     with no backend the ids are off (entry_off), so HOSTLIB_PROBE does
 *     not offer them;
 *   - once admitted, a typed answer, never a decline: a range over its
 *     limit is ACM_E_TOOBIG, a wrapped or null one ACM_E_BADARG;
 *   - session handles: a slot in bits 0-7, a generation in bits 8-31, so a
 *     closed or stale handle is ACM_E_NOSUCH, never another session; at
 *     most ACM_SVC_SESSIONS (4) an instance (ACM_E_LIMIT past that);
 *   - one call into a session at a time (a lock per session);
 *   - the event queue (ACM_EVENTS), its serial, and the hook the services
 *     card's acmedia.event/1 WAIT uses (2.5);
 *   - a reset drops every session (acm_service_reset); a snapshot is
 *     refused while one is open (acm_service_sessions).
 *
 * The backend (gfopenservice's acmedia worker behind its broker, loaded by
 * the board; a fake in acmedia_service_test.c) gets host pointers to ranges
 * already checked, session slots, and posts events from any thread.
 */
#ifndef ACMEDIA_SERVICE_H
#define ACMEDIA_SERVICE_H
#include <stdint.h>

#define ACM_SVC_ABI 1u               /* this header's backend interface */
#define ACM_SVC_SESSIONS 4u          /* sessions an instance (design 2.4, policy default) */
#define ACM_SVC_EVENTS 256u          /* records the queue keeps; the oldest go first */

typedef struct acm_backend {
    uint32_t abi;                    /* ACM_SVC_ABI */
    void *ctx;
    /* each returns as the call does: >= 0, or an ACM_E_* code (acmedia.h) */
    int32_t (*caps)(void *ctx, uint32_t version, uint8_t *out, uint32_t len);
    int32_t (*probe)(void *ctx, const uint8_t *src, uint32_t srclen, uint8_t *out, uint32_t len);   /* a ticket */
    int32_t (*result)(void *ctx, uint32_t ticket, uint8_t *out, uint32_t len);
    /* slot: the broker's (1 to ACM_SVC_SESSIONS); handle: the guest's, for events */
    int32_t (*open)(void *ctx, uint32_t slot, uint32_t handle, const uint8_t *src, uint32_t srclen, uint32_t streams,
                    const uint8_t *video, const uint8_t *audio, uint32_t port, uint32_t flags);
    int32_t (*info)(void *ctx, uint32_t slot, uint8_t *out, uint32_t len);
    int32_t (*control)(void *ctx, uint32_t slot, uint32_t cmd, const uint32_t args[4]);
    int32_t (*frame)(void *ctx, uint32_t slot, uint8_t *out, uint32_t len, uint32_t pitch, uint32_t format, uint32_t w, uint32_t h);
    int32_t (*audio)(void *ctx, uint32_t slot, uint8_t *out, uint32_t len, uint32_t format, uint32_t rate);
    int32_t (*feed)(void *ctx, uint32_t slot, const uint8_t *in, uint32_t len, uint32_t flags);
    void (*close)(void *ctx, uint32_t slot);
    /* a deny's reason, or more than the broker's own line: bytes, or 0 for the broker's line */
    int32_t (*fault)(void *ctx, int32_t code, uint8_t *out, uint32_t len);
} acm_backend;

/* What the runtime gives the backend (libacmedia.so's acmedia_backend_v1 is
 * called with it once, before jit_init):
 *   post    acm_service_post, for its events, from any thread;
 *   source  a source descriptor (ACM_SRC_*, big-endian longs, srclen bytes,
 *           already checked to be guest memory) as a read-only fd the
 *           backend then owns: 0, or an ACM_E_* code. The runtime resolves
 *           it (ACM_SRC_ACFS: openat2 RESOLVE_BENEATH under the unit's
 *           share), so the backend never sees a path. Called on the
 *           emulation thread, inside ACM_PROBE and ACM_OPEN. */
typedef struct acm_host {
    uint32_t abi;                    /* ACM_SVC_ABI */
    void (*post)(uint32_t handle, uint32_t type, uint32_t a, uint32_t b);
    int32_t (*source)(const uint8_t *src, uint32_t srclen, int *fd);
} acm_host;
/* libacmedia.so's one export, "acmedia_backend_v1": its backend, or NULL. */
typedef const acm_backend *(*acm_backend_entry)(const acm_host *host);

/* Before jit_init (the ids' entry_off is read then). NULL: no backend. */
void acm_service_set_backend(const acm_backend *b);
int acm_service_has_backend(void);

/* An event, from the backend, any thread: type ACM_EV_*, its a and b. */
void acm_service_post(uint32_t handle, uint32_t type, uint32_t a, uint32_t b);
/* The serial of the newest event (0: none yet). */
uint32_t acm_service_serial(void);
/* Called (any thread, no lock held) after each event is queued: the
 * services card's acmedia.event/1 completes its waits from it. */
void acm_service_on_event(void (*fn)(uint32_t serial));

/* The machine reset: every session closed, the queue emptied. */
void acm_service_reset(void);
/* Sessions open now (a snapshot is refused while any is). */
int acm_service_sessions(void);

/* for the tests: the twelve ids' bodies called as AC090 would, on guest
 * memory at block. -2: left to the 68k (a gate); else 0 with *d0. */
int acm_service_call_for_test(uint32_t id, uint32_t block, int32_t *d0);
#endif
