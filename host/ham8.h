/*
 * HAM8 for media.decode/1's VFRAME (docs/MEDIA_DECODE.md): a 24-bit picture
 * as AGA hold-and-modify pixels, for an AGA screen to show in eight
 * bitplanes with about 262,000 shades.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef HAM8_H
#define HAM8_H

#include <stdint.h>

#define HAM8_BASE 64              /* base colours */
#define HAM8_PALETTE (HAM8_BASE * 3)

/* The 64 base colours, R, G, B bytes each: fixed, the 4x4x4 cube (each
 * step 85), or chosen for this picture by median cut. */
void ham8_palette(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, int fixed, uint8_t pal[HAM8_PALETTE]);

/* Each pixel as a HAM8 byte: bits 7-6 the control (0 a base colour, 1
 * modify blue, 2 modify red, 3 modify green), bits 5-0 the base colour or
 * the channel's top six bits (the low two are kept from the pixel to the
 * left). Each row starts from base colour 0, as the display does. */
void ham8_map(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, const uint8_t pal[HAM8_PALETTE], uint8_t *out);

/* What the display shows for HAM8 pixels: R, G, B bytes (for tests). */
void ham8_show(const uint8_t *ham, uint32_t w, uint32_t h, const uint8_t pal[HAM8_PALETTE], uint8_t *rgb);

#endif
