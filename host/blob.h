/*
 * Files kept on the host between calls, for media.decode/1 and doc.render/1
 * (docs/MEDIA_DECODE.md, "Files sent once"): an Amiga sends a file once
 * with KEEP, then names it in later calls instead of sending it again.
 *
 * extra[3] of PROBE, DECODE, NDECODE and VOPEN (media.decode/1) and of
 * PROBE, RENDER and TEXT (doc.render/1):
 *   bit 1 (KEEP)    keep buf0's file when the call succeeds; its handle goes
 *                   to buf3 (4 bytes, big-endian, written by the service),
 *                   which the Amiga zeroes first: a host without this
 *                   leaves it 0, and the Amiga goes on sending the file
 *   bit 2 (DROP)    let the named file go after this call
 *   bits 8-31       a kept file: the file, when buf0 is empty
 * A kept file that has gone (pushed out by newer ones, or the host
 * restarted) answers BLOB_GONE; the Amiga sends the file again.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef BLOB_H
#define BLOB_H

#include <stdint.h>

#define BLOB_KEEP  2u
#define BLOB_DROP  4u
#define BLOB_SHIFT 8
#define BLOB_GONE (-7)

/* Release: op 34 in both services; arg the kept file. */
#define BLOB_OP_RELEASE 34

/* Kept files: at most this many, and this many bytes in all; the least
 * recently used goes first. */
#define BLOB_MAX 16
#define BLOB_BYTES (512u << 20)

struct blob_use {
    int slot;                  /* the kept file held for this call; -1 none */
};

/* Before a call: with an empty buf0 (*len 0) and a kept file named in
 * extra3, points *data and *len at its bytes, held until blob_after.
 * 0, or BLOB_GONE. */
int blob_before(uint32_t extra3, const uint8_t **data, uint32_t *len, struct blob_use *u);

/* After a call: with KEEP and ok, keeps the file (a copy of data, or the
 * one already named) and writes its handle to out when room >= 4 (sets
 * *written); with DROP, lets the named file go; then lets the hold go. */
void blob_after(int ok, uint32_t extra3, const uint8_t *data, uint32_t len, uint8_t *out, uint32_t room,
                uint32_t *written, struct blob_use *u);

/* Lets a kept file go. 0, or BLOB_GONE when there is no such file. */
int blob_release(uint32_t handle);

/* Kept files now, and their bytes (for tests). */
void blob_stats(uint32_t *count, uint64_t *bytes);

#endif
