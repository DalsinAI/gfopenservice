/*
 * media.decode/1's NDECODE (op 33): pictures decoded on the host by the
 * Amiga's own decoders, built from gfopendatatypes' sources (gfopenpng's
 * pngdecode.c, gfopentga's tgadecode.c, gfopengif's gifdecode.c, and
 * libwebp 1.6.0 with webp.datatype's patches), so the pixels are the ones
 * the 68k would have made, byte for byte (docs/MEDIA_DECODE.md, "The
 * Amiga's own decoders"). Built in when host/build_native.sh has built
 * them (MD_NATIVE); without them NDECODE answers BADREQUEST and the
 * Amiga decodes on its own CPU.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef MEDIA_NATIVE_H
#define MEDIA_NATIVE_H

#include <stdint.h>
#include "media_decode.h"

/* NDECODE's decoders (extra[0]) and modes (extra[1]). */
#define MD_NATIVE_PNG  0x504e4720u   /* 'PNG ': ARGB */
#define MD_NATIVE_TGA  0x54474120u   /* 'TGA ': ARGB */
#define MD_NATIVE_GIF  0x47494620u   /* 'GIF ': mode 0 ARGB of frame arg's screen; 1 frame 0's colour indexes */
#define MD_NATIVE_WEBP 0x57454250u   /* 'WEBP': ARGB; an animation's first frame at its place */

#define MD_NATIVE_GIF_ARGB 0
#define MD_NATIVE_GIF_LUT8 1

/* NDECODE's info in buf2: four big-endian u32s. */
#define MD_NATIVE_INFO 16
#define MD_NATIVE_ALPHA 1            /* info[2] bit 0: the picture has alpha */

/* 1 when built with the decoders. */
int md_native_ready(void);
int md_native_decode(uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4], uint32_t *result, uint32_t *aux);

#endif
