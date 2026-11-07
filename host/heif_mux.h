/*
 * HEIF's container around HEVC (ISO/IEC 23008-12): one picture, and an
 * alpha plane beside it when there is one, as a .heic file. The HEVC is
 * FFmpeg's (media_encode.c); this only writes the boxes.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef HEIF_MUX_H
#define HEIF_MUX_H

#include <stddef.h>
#include <stdint.h>

struct heif_picture {
    const uint8_t *hevc;         /* Annex B: start codes, VPS, SPS, PPS, the picture's slices */
    size_t hevc_len;
    const uint8_t *alpha;        /* the same for the alpha plane (its luma), or NULL */
    size_t alpha_len;
    uint32_t width, height;      /* the picture's own size */
    uint32_t coded_width, coded_height;   /* the size coded (even): cropped to width x height */
    int full_range;              /* the colour's YCbCr is full range (else video range) */
};

/* The .heic file, malloc'd, into *out and *len; 0, or -1 for HEVC it can't
 * read (no VPS, SPS, PPS or slice). */
int heif_write(const struct heif_picture *p, uint8_t **out, size_t *len);

/* The fields of an HEVC SPS that hvcC repeats, from one SPS NAL unit (with
 * its two-byte header). 0, or -1 when it is not one. */
struct hevc_sps_info {
    uint8_t ptl[12];             /* general profile space, tier, profile, compatibility, constraints, level */
    uint32_t chroma_format, bit_depth_luma, bit_depth_chroma, width, height;
    uint32_t sub_layers, temporal_id_nested;
};
int hevc_parse_sps(const uint8_t *nal, size_t n, struct hevc_sps_info *s);

#endif
