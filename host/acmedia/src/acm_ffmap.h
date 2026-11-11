/* FFmpeg ids to ACMedia tags (worker only). MIT, Copyright (c) 2026 AmiCradle Team. */
#ifndef ACM_FFMAP_H
#define ACM_FFMAP_H
#include <stdint.h>
#include <libavcodec/codec_id.h>
uint32_t acm_tag_of(enum AVCodecID id);           /* 0: not ours */
uint32_t acm_container_tag(const char *demuxer);  /* by the demuxer's name */
#endif
