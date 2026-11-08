/*
 * media.encode/1: pictures, sounds and films written by FFmpeg on the host,
 * for the Amiga's datatypes (docs/MEDIA_ENCODE.md). The Amiga sends the
 * object's pixels, samples or frames; the host runs the ffmpeg program in a
 * process of its own and hands the file back in pieces. Nothing here links
 * FFmpeg: its encoders (and their licences and patents) stay in that
 * process. HEIC's container is written here (heif_mux.c) around the HEVC
 * FFmpeg makes.
 *
 * The op numbers start at 16 so they never meet media.decode/1's (1 and up)
 * even where one table serves both services.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef MEDIA_ENCODE_H
#define MEDIA_ENCODE_H

#include <stdint.h>
#include "media_decode.h"   /* struct md_buffer and the status numbers */

#define ME_FORMATS 16   /* buf1 (out) the formats it writes, big-endian u32s; result how many */
#define ME_BEGIN   17   /* buf0 the settings (ME_SETTINGS_SIZE bytes) -> result the job */
#define ME_PUT     18   /* arg the job, extra[0] the stream (ME_STREAM_*), buf0 the next bytes -> result that
                           stream's bytes so far */
#define ME_FINISH  19   /* arg the job -> buf1 (out, optional) ME_INFO_SIZE bytes of info; result the file's
                           bytes. Starts FFmpeg and waits for it at most ME_FINISH_WAIT seconds: aux 1 (and
                           result 0) when it is still at work, and FINISH is sent again */
#define ME_READ    20   /* arg the job, extra[0] the offset -> buf1 (out) the file's bytes; result how many (0 at
                           the end) */
#define ME_CLOSE   21   /* arg the job */

#define ME_STREAM_PICTURE 0   /* a picture's pixels, or a film's frames one after the other */
#define ME_STREAM_SOUND   1   /* the samples, interleaved */

/* The settings, big-endian u32s at these offsets. */
#define ME_SETTINGS_SIZE 48
#define ME_S_VERSION   0    /* 1 */
#define ME_S_FORMAT    4    /* the file to write: ME_FORMAT_* */
#define ME_S_WIDTH     8    /* pictures and films */
#define ME_S_HEIGHT   12
#define ME_S_PIXELS   16    /* ME_PIX_* */
#define ME_S_FPS1000  20    /* films: frames a second x 1000 */
#define ME_S_RATE     24    /* samples a second; 0: no sound (a film without one) */
#define ME_S_CHANNELS 28    /* 1 or 2 */
#define ME_S_SAMPLES  32    /* ME_SAMPLE_* */
#define ME_S_QUALITY  36    /* 1 (smallest) to 100 (best); 0: the format's default */
#define ME_S_FLAGS    40    /* ME_F_* */
#define ME_S_RESERVED 44    /* 0 */

#define ME_PIX_ARGB  0      /* 4 bytes a pixel: A, R, G, B (picture.datatype's PBPAFMT_ARGB) */
#define ME_PIX_RGB   1      /* 3 bytes a pixel: R, G, B */

#define ME_SAMPLE_S16 0     /* 16-bit signed, big-endian */
#define ME_SAMPLE_S8  1     /* 8-bit signed */

#define ME_F_ALPHA 1        /* pictures: keep the alpha (HEIC: an alpha plane beside the picture) */

/* What it writes. The Amiga names the file, not the codec. */
#define ME_FORMAT_HEIC 0x48454943u   /* 'HEIC': HEVC still picture in HEIF */
#define ME_FORMAT_M4A  0x4d344120u   /* 'M4A ': AAC-LC in MP4 */
#define ME_FORMAT_AAC  0x41414320u   /* 'AAC ': AAC-LC as ADTS */
#define ME_FORMAT_WMA  0x574d4120u   /* 'WMA ': Windows Media Audio 2 in ASF */
#define ME_FORMAT_HEVC 0x48455643u   /* 'HEVC': HEVC film in MP4, AAC-LC sound */
#define ME_FORMAT_WMV  0x574d5620u   /* 'WMV ': Windows Media Video 8 in ASF, WMA 2 sound */

/* FINISH's info, big-endian u32s: the format, the file's bytes, width,
 * height, frames, sample frames. */
#define ME_INFO_SIZE 24

#define ME_FINISH_WAIT 20     /* seconds; a LAN request may go 60 without an answer */

/* Limits. */
#define ME_MAX_JOBS     8              /* open at once; the oldest idle one goes */
#define ME_MAX_SIDE     8192           /* a picture's side (HEIC: one HEVC picture) */
#define ME_MAX_FILM_SIDE 4096
#define ME_MAX_STREAM   0xfff00000u    /* bytes in one stream */

int me_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux);

#endif
