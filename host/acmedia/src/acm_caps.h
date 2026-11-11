/* ACMedia's capability probe (acm_caps.c). MIT, Copyright (c) 2026 AmiCradle Team. */
#ifndef ACM_CAPS_H
#define ACM_CAPS_H
#include <libavutil/buffer.h>
#include "acm_proto.h"
/* 1 when a V4L2 decoder is here: stateless HEVC (stateless 1) or stateful H.264 (0) */
int acm_caps_has_v4l2(int stateless);
void acm_caps_fill(struct acm_caps *c, uint32_t paths, AVBufferRef *vaapi);
#endif
