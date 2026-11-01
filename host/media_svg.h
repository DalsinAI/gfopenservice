/*
 * media.decode/1, SVG: drawn on the host by librsvg and cairo, loaded when
 * first needed, so a host without them still offers everything else.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef MEDIA_SVG_H
#define MEDIA_SVG_H

#include <stdint.h>
#include "media_decode.h"

/* 1 when the bytes (or the hint "svg" / "svgz") are an SVG and librsvg is here. */
int md_svg_is(const uint8_t *d, uint32_t n, const char *hint);

/* PROBE or DECODE for an SVG. extra[0..1] the size wanted; with MD_EXACT in
 * extra[3] exactly that size (one of them 0: kept to the SVG's aspect; both
 * 0: the SVG's own size), else the largest size, as for any picture. */
int md_svg_call(uint16_t op, const uint32_t extra[4], struct md_buffer buf[4], uint32_t *result, uint32_t *aux);

#endif
