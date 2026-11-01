/*
 * media.decode/1: pictures for an Amiga's datatypes, decoded on the host
 * (docs/MEDIA_DECODE.md). One call per request; no state between calls.
 * The emulator's service host and the LAN Cradle both call this.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef MEDIA_DECODE_H
#define MEDIA_DECODE_H

#include <stdint.h>

#define MD_PROBE  1   /* buf0 file, extra[0..1] largest size -> buf1 info (24 bytes); result width, aux height */
#define MD_DECODE 2   /* arg frame, buf0 file, extra[0..1] largest size -> buf1 ARGB; result width, aux height */
#define MD_VOPEN  3   /* buf0 file, extra[0..1] largest size -> buf1 info; result handle, aux fps x 1000 */
#define MD_VFRAME 4   /* arg handle, extra[0] frame, extra[1] 0 chunky / 1 RGB / 2 HAM8, extra[2] bit 0 fixed base
                         colours (HAM8) -> buf1; result the frame */
#define MD_VCLOSE 5   /* arg handle */
#define MD_VOPENURL 6 /* buf0 an http:// or https:// URL, extra[0..1] largest size -> buf1 info; result handle,
                         aux fps x 1000. Read forward only (MD_FLAG_LIVE) */
#define MD_VSOUND 7   /* arg handle, extra[0] frame, extra[1] rate -> buf1 that frame's sound, 8-bit signed mono;
                         result the samples, aux the rate */
#define MD_VUPLOAD 8  /* arg upload (0: a new one), buf0 its next part, extra[0] bit 0 the last part -> result
                         the upload, aux its bytes so far. VOPEN with an empty buf0 and extra[2] the upload opens it */

#define MD_KIND_PICTURE 1
#define MD_KIND_ANIMATION 2
#define MD_KIND_SOUND   3

#define MD_FORMAT_AVIF 0x41564946u   /* 'AVIF' */
#define MD_FORMAT_HEIC 0x48454943u   /* 'HEIC' */
#define MD_FORMAT_JPEG 0x4a504547u   /* 'JPEG' */
#define MD_FORMAT_PNG  0x504e4720u   /* 'PNG ' */
#define MD_FORMAT_GIF  0x47494620u   /* 'GIF ': the first frame */
#define MD_FORMAT_WEBP 0x57454250u   /* 'WEBP' */
#define MD_FORMAT_JXL  0x4a584c20u   /* 'JXL ' */
#define MD_FORMAT_EXR  0x45585220u   /* 'EXR ' */
#define MD_FORMAT_HDR  0x48445220u   /* 'HDR ' */
#define MD_FORMAT_PSD  0x50534420u   /* 'PSD ' */
#define MD_FORMAT_QOI  0x514f4920u   /* 'QOI ' */
#define MD_FORMAT_DDS  0x44445320u   /* 'DDS ' */
#define MD_FORMAT_J2K  0x4a324b20u   /* 'J2K ' */
#define MD_FORMAT_TIFF 0x54494646u   /* 'TIFF' */
#define MD_FORMAT_DPX  0x44505820u   /* 'DPX ' */
#define MD_FORMAT_PCX  0x50435820u   /* 'PCX ' */
#define MD_FORMAT_SGI  0x53474920u   /* 'SGI ' */
#define MD_FORMAT_ICO  0x49434f20u   /* 'ICO ': a Windows icon file's first icon */
#define MD_FORMAT_SVG  0x53564720u   /* 'SVG ': drawn by librsvg, at the size asked */
#define MD_FORMAT_STILL  0x5354494cu /* 'STIL': another picture FFmpeg reads */
#define MD_FORMAT_RAW    0x52415720u /* 'RAW ': camera RAW, through LibRaw */
#define MD_FORMAT_MAGICK 0x494d474bu /* 'IMGK': another picture ImageMagick reads (else the hint, 'TGA ') */
#define MD_FORMAT_ORA    0x4f524120u /* 'ORA ': OpenRaster's flattened picture */
#define MD_FORMAT_KRA    0x4b524120u /* 'KRA ': Krita's */
#define MD_FORMAT_CBZ    0x43425a20u /* 'CBZ ': a comic book's first page */
#define MD_FORMAT_MIDI   0x4d494449u /* 'MIDI', rendered by FluidSynth */
#define MD_FORMAT_SID    0x53494420u /* 'SID ', rendered by sidplayfp */
#define MD_FORMAT_MODULE 0x4d4f4420u /* 'MOD ': tracker modules, through libopenmpt */
#define MD_FORMAT_CHIPTUNE 0x474d4520u /* 'GME ': NSF, SPC, VGM... through Game Music Emu */
#define MD_FORMAT_FLAC   0x464c4143u /* 'FLAC' */
#define MD_FORMAT_VORBIS 0x564f5242u /* 'VORB' */
#define MD_FORMAT_OPUS   0x4f505553u /* 'OPUS' */
#define MD_FORMAT_MP3    0x4d503320u /* 'MP3 ' */
#define MD_FORMAT_AAC    0x41414320u /* 'AAC ' */
#define MD_FORMAT_ALAC   0x414c4143u /* 'ALAC' */
#define MD_FORMAT_WMA    0x574d4120u /* 'WMA ' */
#define MD_FORMAT_SOUND  0x534f554eu /* 'SOUN': another sound FFmpeg reads */
#define MD_FORMAT_H264   0x48323634u /* 'H264' */
#define MD_FORMAT_HEVC   0x48455643u /* 'HEVC' */
#define MD_FORMAT_AV1    0x41563120u /* 'AV1 ' */
#define MD_FORMAT_VP8    0x56503820u /* 'VP8 ' */
#define MD_FORMAT_VP9    0x56503920u /* 'VP9 ' */
#define MD_FORMAT_MPEG4  0x4d504734u /* 'MPG4' */
#define MD_FORMAT_MPEG2  0x4d504732u /* 'MPG2': MPEG-1 and MPEG-2 */
#define MD_FORMAT_WMV    0x574d5620u /* 'WMV ' */
#define MD_FORMAT_MJPEG  0x4d4a5047u /* 'MJPG' */
#define MD_FORMAT_THEORA 0x54484f52u /* 'THOR' */
#define MD_FORMAT_VIDEO  0x56494445u /* 'VIDE': another video FFmpeg reads */

#define MD_FLAG_ALPHA 1
#define MD_FLAG_SOUND 2   /* a video with a sound track */
#define MD_FLAG_LOOP  4   /* an animation (GIF, APNG) that asks to play over and over */
#define MD_FLAG_LIVE  8   /* a stream from a URL: frames by the clock, never back; frames 0 when it never ends */

/* VFRAME extra[2] for HAM8: the fixed 4x4x4 cube as base colours, not the picture's own. */
#define MD_HAM8_FIXED 1

#define MD_INFO_SIZE 24

/* extra[3] for PROBE and DECODE of a picture: draw an SVG at exactly
 * extra[0] x extra[1] (one 0: kept to its aspect; both 0: its own size). */
#define MD_EXACT 1

/* Status, as the services card's. */
#define MD_OK          0
#define MD_BADREQUEST (-2)
#define MD_TOOSMALL   (-4)
#define MD_HOSTERROR  (-5)

struct md_buffer {
    const uint8_t *in;     /* the Amiga's bytes, for a buffer the service reads */
    uint8_t *out;          /* where to write, for a buffer the service writes */
    uint32_t length;       /* in: its length; out: the room */
    uint32_t written;      /* out: bytes written */
};

/* Runs one request. Returns the status; sets *result and *aux. */
int md_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux);

/* Big-endian u32 at p. */
void md_put32(uint8_t *p, uint32_t v);

/* The size a w x h picture is written at, to fit inside maxw x maxh (0: any). */
void md_fit(uint32_t w, uint32_t h, uint32_t maxw, uint32_t maxh, uint32_t *ow, uint32_t *oh);

#endif
