/*
 * doc.render/1: office documents for an Amiga's datatypes, laid out by
 * LibreOffice on the host and sent back as pictures of their pages
 * (docs/DOC_RENDER.md). The emulator's service host and the LAN Cradle
 * both call this. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef DOC_RENDER_H
#define DOC_RENDER_H

#include <stdint.h>

#define DR_PROBE  1   /* buf0 file, extra[0..1] largest page size, extra[2] extension hint -> buf1 info (24 bytes); result pages */
#define DR_RENDER 2   /* arg page, buf0 file, extra[0..1] largest page size -> buf1 ARGB; result width, aux height */
#define DR_TEXT   3   /* buf0 file -> buf1 the text, ISO-8859-1, lines ending in LF; result bytes */

#define DR_RELEASE 34 /* arg a file kept with KEEP (blob.h): let it go */

/* extra[3] of PROBE, RENDER and TEXT: files sent once (blob.h). */
#define DR_GONE (-7)  /* the kept file named is no longer here: send it again */

#define DR_KIND_DOCUMENT 4

/* Status, as the services card's. */
#define DR_OK          0
#define DR_BADREQUEST (-2)
#define DR_TOOSMALL   (-4)
#define DR_HOSTERROR  (-5)

struct dr_buffer {
    const uint8_t *in;     /* the Amiga's bytes, for a buffer the service reads */
    uint8_t *out;          /* where to write, for a buffer the service writes */
    uint32_t length;       /* in: its length; out: the room */
    uint32_t written;      /* out: bytes written */
};

/* Runs one request. Returns the status; sets *result and *aux. */
int dr_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct dr_buffer buf[4],
            uint32_t *result, uint32_t *aux);

/* The format's four letters ('DOCX', 'ODT ', 'EPUB', ...), or 0 when not a
 * document; hint is the extension in capitals ('CSV ') for text formats. */
uint32_t dr_sniff(const uint8_t *d, uint32_t n, uint32_t hint);

#endif
