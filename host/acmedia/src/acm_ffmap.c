/* FFmpeg's codec ids and demuxers to ACMedia's tags (worker only).
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <string.h>
#include <libavcodec/avcodec.h>
#include "acm_codecs.h"
#include "acm_ffmap.h"

uint32_t acm_tag_of(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return ACM_T_H264;
    case AV_CODEC_ID_HEVC: return ACM_T_HEVC;
    case AV_CODEC_ID_AV1: return ACM_T_AV1;
    case AV_CODEC_ID_VP8: return ACM_T_VP8;
    case AV_CODEC_ID_VP9: return ACM_T_VP9;
    case AV_CODEC_ID_MPEG1VIDEO: case AV_CODEC_ID_MPEG2VIDEO: return ACM_T_MPEG2;
    case AV_CODEC_ID_MPEG4: case AV_CODEC_ID_H263: case AV_CODEC_ID_MSMPEG4V1:
    case AV_CODEC_ID_MSMPEG4V2: case AV_CODEC_ID_MSMPEG4V3: return ACM_T_MPEG4;
    case AV_CODEC_ID_WMV1: case AV_CODEC_ID_WMV2: return ACM_T_WMV;
    case AV_CODEC_ID_WMV3: case AV_CODEC_ID_VC1: return ACM_T_VC1;
    case AV_CODEC_ID_THEORA: return ACM_T_THEORA;
    case AV_CODEC_ID_MJPEG: return ACM_T_MJPEG;
    case AV_CODEC_ID_PRORES: return ACM_T_PRORES;
    case AV_CODEC_ID_FFV1: return ACM_T_FFV1;
    case AV_CODEC_ID_CINEPAK: case AV_CODEC_ID_INDEO3: case AV_CODEC_ID_INDEO4:
    case AV_CODEC_ID_INDEO5: case AV_CODEC_ID_MSVIDEO1: case AV_CODEC_ID_MSRLE:
    case AV_CODEC_ID_QTRLE: case AV_CODEC_ID_RPZA: case AV_CODEC_ID_SMC:
    case AV_CODEC_ID_SVQ1: case AV_CODEC_ID_SVQ3: case AV_CODEC_ID_FLIC:
    case AV_CODEC_ID_SMACKVIDEO: case AV_CODEC_ID_ROQ: return ACM_T_CDROM;
    case AV_CODEC_ID_CDXL: return ACM_T_CDXL;
    case AV_CODEC_ID_IFF_ILBM: return ACM_T_ILBM;
    case AV_CODEC_ID_ANM: return ACM_T_ANM;
    case AV_CODEC_ID_8SVX_FIB: case AV_CODEC_ID_8SVX_EXP: case AV_CODEC_ID_PCM_S8_PLANAR:
        return ACM_T_8SVX;
    case AV_CODEC_ID_MP1: case AV_CODEC_ID_MP2: case AV_CODEC_ID_MP3: return ACM_T_MP3;
    case AV_CODEC_ID_AAC: case AV_CODEC_ID_AAC_LATM: return ACM_T_AAC;
    case AV_CODEC_ID_AC3: return ACM_T_AC3;
    case AV_CODEC_ID_EAC3: return ACM_T_EAC3;
    case AV_CODEC_ID_DTS: return ACM_T_DTS;
    case AV_CODEC_ID_FLAC: return ACM_T_FLAC;
    case AV_CODEC_ID_VORBIS: return ACM_T_VORBIS;
    case AV_CODEC_ID_OPUS: return ACM_T_OPUS;
    case AV_CODEC_ID_ALAC: return ACM_T_ALAC;
    case AV_CODEC_ID_WMAV1: case AV_CODEC_ID_WMAV2: return ACM_T_WMA;
    case AV_CODEC_ID_WAVPACK: return ACM_T_WAVPK;
    case AV_CODEC_ID_TTA: return ACM_T_TTA;
    case AV_CODEC_ID_APE: return ACM_T_APE;
    case AV_CODEC_ID_SMACKAUDIO: case AV_CODEC_ID_ADPCM_IMA_WAV: case AV_CODEC_ID_ADPCM_MS:
    case AV_CODEC_ID_PCM_S8: case AV_CODEC_ID_PCM_U8: case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE: case AV_CODEC_ID_PCM_S24LE: case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32LE: case AV_CODEC_ID_PCM_S32BE: case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F32BE: case AV_CODEC_ID_PCM_ALAW: case AV_CODEC_ID_PCM_MULAW:
        return ACM_T_PCM;
    case AV_CODEC_ID_SUBRIP: case AV_CODEC_ID_ASS: case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_MOV_TEXT: case AV_CODEC_ID_WEBVTT: return ACM_T_SUBTEXT;
    case AV_CODEC_ID_DVD_SUBTITLE: case AV_CODEC_ID_DVB_SUBTITLE:
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE: return ACM_T_SUBPIC;
    default: return 0;
    }
}

uint32_t acm_container_tag(const char *demuxer)
{
    static const struct { const char *n; uint32_t t; } m[] = {
        { "mov,mp4,m4a,3gp,3g2,mj2", ACM_TAG('M','P','4',' ') },
        { "matroska,webm", ACM_TAG('M','K','V',' ') },
        { "avi", ACM_TAG('A','V','I',' ') }, { "mpeg", ACM_TAG('M','P','S',' ') },
        { "mpegts", ACM_TAG('M','P','T','S') }, { "ogg", ACM_TAG('O','G','G',' ') },
        { "wav", ACM_TAG('W','A','V','E') }, { "aiff", ACM_TAG('A','I','F','F') },
        { "asf", ACM_TAG('A','S','F',' ') }, { "flv", ACM_TAG('F','L','V',' ') },
        { "iff", ACM_TAG('I','F','F',' ') }, { "cdxl", ACM_TAG('C','D','X','L') },
        { "anm", ACM_TAG('A','N','M',' ') }, { "flic", ACM_TAG('F','L','I','C') },
        { "ivf", ACM_TAG('I','V','F',' ') }, { "libopenmpt", ACM_TAG('M','O','D',' ') },
        { "mp3", ACM_TAG('M','P','3',' ') }, { "flac", ACM_TAG('F','L','A','C') },
        { "h264", ACM_TAG('H','2','6','4') }, { "hevc", ACM_TAG('H','E','V','C') },
        { "obu", ACM_TAG('O','B','U',' ') },
    };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
        if (!strcmp(m[i].n, demuxer)) return m[i].t;
    return ACM_TAG('O','T','H','R');
}
