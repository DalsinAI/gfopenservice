/* ACMedia's tag list (acm_codecs.h), shared by the worker and the broker.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <string.h>
#include "acm_codecs.h"

const uint32_t acm_all_tags[] = {
    ACM_T_H264, ACM_T_HEVC, ACM_T_AV1, ACM_T_VP8, ACM_T_VP9, ACM_T_MPEG2, ACM_T_MPEG4,
    ACM_T_WMV, ACM_T_VC1, ACM_T_THEORA, ACM_T_MJPEG, ACM_T_PRORES, ACM_T_FFV1, ACM_T_CDROM,
    ACM_T_CDXL, ACM_T_ILBM, ACM_T_ANM, ACM_T_8SVX, ACM_T_MODULE,
    ACM_T_MP3, ACM_T_AAC, ACM_T_AC3, ACM_T_EAC3, ACM_T_DTS, ACM_T_FLAC, ACM_T_VORBIS,
    ACM_T_OPUS, ACM_T_ALAC, ACM_T_WMA, ACM_T_WAVPK, ACM_T_TTA, ACM_T_APE, ACM_T_PCM,
    ACM_T_SUBTEXT, ACM_T_SUBPIC,
};
const unsigned acm_all_tags_n = sizeof(acm_all_tags) / sizeof(acm_all_tags[0]);

uint32_t acm_tag_parse(const char *s)
{
    char b[4] = { ' ', ' ', ' ', ' ' };
    size_t n = strlen(s);
    if (n == 0 || n > 4) return 0;
    memcpy(b, s, n);
    uint32_t t = ACM_TAG(b[0], b[1], b[2], b[3]);
    for (unsigned i = 0; i < acm_all_tags_n; i++)
        if (acm_all_tags[i] == t) return t;
    return 0;
}
