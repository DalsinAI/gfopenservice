/*
 * ACMedia's codec tags: four letters, ours, the ones media.decode/1 already
 * uses (MD_FORMAT_*), plus the Amiga's. FFmpeg's codec ids stay inside the
 * worker (acm_codecs.c); a decoder without a tag here is never opened.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef ACM_CODECS_H
#define ACM_CODECS_H

#include <stdint.h>

#define ACM_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Video */
#define ACM_T_H264   ACM_TAG('H','2','6','4')
#define ACM_T_HEVC   ACM_TAG('H','E','V','C')
#define ACM_T_AV1    ACM_TAG('A','V','1',' ')
#define ACM_T_VP8    ACM_TAG('V','P','8',' ')
#define ACM_T_VP9    ACM_TAG('V','P','9',' ')
#define ACM_T_MPEG2  ACM_TAG('M','P','G','2')   /* MPEG-1 and MPEG-2 video */
#define ACM_T_MPEG4  ACM_TAG('M','P','G','4')   /* MPEG-4 Part 2, H.263, MS-MPEG4 */
#define ACM_T_WMV    ACM_TAG('W','M','V',' ')   /* WMV 7/8 */
#define ACM_T_VC1    ACM_TAG('V','C','1',' ')   /* VC-1, WMV 9 */
#define ACM_T_THEORA ACM_TAG('T','H','O','R')
#define ACM_T_MJPEG  ACM_TAG('M','J','P','G')
#define ACM_T_PRORES ACM_TAG('P','R','E','S')
#define ACM_T_FFV1   ACM_TAG('F','F','V','1')
#define ACM_T_CDROM  ACM_TAG('C','D','R','M')   /* the CD-ROM era: Cinepak, Indeo, MS Video 1, RLE, SMC, RPZA, Sorenson, FLIC, Smacker, RoQ */
/* Amiga */
#define ACM_T_CDXL   ACM_TAG('C','D','X','L')
#define ACM_T_ILBM   ACM_TAG('I','L','B','M')   /* ILBM, ACBM, DEEP, PBM, RGB8, RGBN, and ANIM's frames */
#define ACM_T_ANM    ACM_TAG('A','N','M',' ')   /* Deluxe Paint Animation */
#define ACM_T_8SVX   ACM_TAG('8','S','V','X')
#define ACM_T_MODULE ACM_TAG('M','O','D',' ')   /* trackers, libopenmpt */
/* Audio */
#define ACM_T_MP3    ACM_TAG('M','P','3',' ')   /* MPEG audio layers 1-3 */
#define ACM_T_AAC    ACM_TAG('A','A','C',' ')
#define ACM_T_AC3    ACM_TAG('A','C','3',' ')
#define ACM_T_EAC3   ACM_TAG('E','A','C','3')
#define ACM_T_DTS    ACM_TAG('D','T','S',' ')
#define ACM_T_FLAC   ACM_TAG('F','L','A','C')
#define ACM_T_VORBIS ACM_TAG('V','O','R','B')
#define ACM_T_OPUS   ACM_TAG('O','P','U','S')
#define ACM_T_ALAC   ACM_TAG('A','L','A','C')
#define ACM_T_WMA    ACM_TAG('W','M','A',' ')
#define ACM_T_WAVPK  ACM_TAG('W','V','P','K')
#define ACM_T_TTA    ACM_TAG('T','T','A',' ')
#define ACM_T_APE    ACM_TAG('A','P','E',' ')
#define ACM_T_PCM    ACM_TAG('P','C','M',' ')   /* PCM and ADPCM */
/* Subtitles */
#define ACM_T_SUBTEXT ACM_TAG('S','U','B','T')  /* SRT, ASS/SSA, mov_text, WebVTT */
#define ACM_T_SUBPIC  ACM_TAG('S','U','B','P')  /* DVD, DVB, PGS */

/* "H264" from a tag (buf of 5) */
static inline const char *acm_tagstr(uint32_t t, char *buf)
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(t >> (24 - 8 * i));
        buf[i] = (c >= 32 && c < 127) ? c : '?';
    }
    buf[4] = 0;
    return buf;
}

/* Every tag we know, for a policy that allows everything */
extern const uint32_t acm_all_tags[];
extern const unsigned acm_all_tags_n;
uint32_t acm_tag_parse(const char *s);   /* "HEVC" -> tag, 0 when not one of ours */

#endif
