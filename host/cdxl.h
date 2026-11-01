/*
 * media.cdxl/1: any movie FFmpeg reads, transcoded on the host to CDXL, the
 * Amiga's own streaming video format, for the machine that will play it
 * (docs/MEDIA_CDXL.md). MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef CDXL_H
#define CDXL_H

#include <stdint.h>
#include "media_decode.h"

#define CX_CONVERT 1   /* buf0 the movie, extra the settings -> buf1 info (24 bytes); result handle, aux bytes */
#define CX_READ    2   /* arg handle, extra[0] offset -> buf1 the CDXL bytes; result bytes written */
#define CX_CLOSE   3   /* arg handle */

/* extra[0]: the preset. */
#define CX_ECS     0   /* 32 colours, 320 x 180, 12 frames a second, mono */
#define CX_ECS_HAM 1   /* HAM6, 320 x 180, 12 frames a second, mono */
#define CX_AGA     2   /* 256 colours, 320 x 180, 15 frames a second, mono */
#define CX_AGA_HAM 3   /* HAM8, 320 x 180, 15 frames a second, mono */
#define CX_RTG     4   /* 24-bit chunky, 640 x 360, 25 frames a second, stereo */

/* extra[1]: the largest width << 16 | height (0: the preset's).
 * extra[2]: bits 0-7 frames a second, bits 8-15 bitplanes for the colour
 *           presets (1 to 8), 0: the preset's.
 * extra[3]: bits 0-17 the sound's rate (0: the preset's), bit 30 no sound,
 *           bit 31 stereo. */
#define CX_NO_SOUND (1u << 30)
#define CX_STEREO   (1u << 31)

/* info (big-endian u32s): 'CDXL', frames, width, height, planes | mode << 8
 * (0 colours, 1 HAM, 2 chunky 24-bit) | stereo << 16, frames a second << 24
 * | sound rate. */
#define CX_INFO_SIZE 24

int cx_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux);

#endif
