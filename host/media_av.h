/*
 * media.decode/1, the FFmpeg half (media_av.c): sound and video.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef MEDIA_AV_H
#define MEDIA_AV_H

#include <stdint.h>
#include "media_decode.h"

/* What FFmpeg makes of the file. */
#define MD_AV_NONE  0
#define MD_AV_STILL 1     /* a still picture (JPEG XL, EXR, PSD, ...) */
#define MD_AV_MEDIA 2     /* a sound or a video */
int md_is_av(const uint8_t *d, uint32_t n);
/* A still picture's first image, full size, as ARGB (malloc'd) when decode;
 * just its format, flags and size otherwise. */
int md_still_load(const uint8_t *d, uint32_t n, int decode, uint32_t *format, uint32_t *flags,
                  uint32_t *width, uint32_t *height, uint8_t **argb);
/* PROBE and DECODE for a sound: extra[0] most channels, extra[1] highest rate. */
int md_sound_probe(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                   uint32_t *result, uint32_t *aux);
int md_sound_decode(const struct md_buffer *file, uint32_t first, const uint32_t extra[4], struct md_buffer *out,
                    uint32_t *result, uint32_t *aux);

/* Video: VOPEN keeps the file and answers info, result the handle, aux
 * frames a second x 1000; VFRAME (extra[0] the frame, extra[1] 0 for 8-bit
 * dithered, 1 for 24-bit RGB, 2 for HAM8) gives one; VCLOSE lets it go. */
int md_video_open(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                  uint32_t *result, uint32_t *aux);
int md_video_frame(uint32_t handle, const uint32_t extra[4], struct md_buffer *out, uint32_t *result, uint32_t *aux);
int md_video_close(uint32_t handle);
/* VOPENURL: a stream from the network, read forward only. */
int md_video_open_url(const struct md_buffer *url, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                      uint32_t *result, uint32_t *aux);
/* VSOUND: frame extra[0]'s sound, 8-bit signed mono at extra[1] Hz. */
int md_video_sound(uint32_t handle, const uint32_t extra[4], struct md_buffer *out, uint32_t *result, uint32_t *aux);
/* VUPLOAD: a file in parts, for VOPEN. */
int md_video_upload(uint32_t handle, const struct md_buffer *part, const uint32_t extra[4], uint32_t *result,
                    uint32_t *aux);

#endif
