/*
 * media.decode/1, the half that runs host tools (media_tool.c): camera RAW
 * through LibRaw's dcraw_emu, the raster formats ImageMagick reads (XCF,
 * TGA, PCX, ...; md_magick_coder), MIDI through FluidSynth and SID tunes
 * through sidplayfp.
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#ifndef MEDIA_TOOL_H
#define MEDIA_TOOL_H

#include <stdint.h>

/* The extension hint from extra[2] ('CR2 ' -> "cr2"); "" when none. */
void md_hint(uint32_t hint, char out[8]);
/* 1 when the hint or the bytes say camera RAW. */
int md_is_raw(const uint8_t *d, uint32_t n, const char *hint);
/* The ImageMagick coder named for these bytes ("PNG"), from their
 * signature, or from the hint for the few formats without one; NULL when
 * none, and ImageMagick isn't run. */
const char *md_magick_coder(const uint8_t *d, uint32_t n, const char *hint);
/* A picture through dcraw_emu (raw) or ImageMagick, full size, as ARGB
 * (malloc'd); the result is kept in the cache, so a PROBE then a DECODE
 * converts once. */
int md_tool_picture(const uint8_t *d, uint32_t n, const char *hint, int raw, uint32_t *format,
                    uint32_t *flags, uint32_t *width, uint32_t *height, uint8_t **argb);

/* The picture inside a ZIP (malloc'd): OpenRaster's and Krita's flattened
 * picture, or a comic book's first page (hint "cbz"); NULL when none. */
uint8_t *md_zip_picture(const uint8_t *d, uint32_t n, const char *hint, uint32_t *format, uint32_t *len);

/* 'MIDI' or 'SID ' when the bytes are a tune the host renders, else 0. */
uint32_t md_tune_sniff(const uint8_t *d, uint32_t n);
/* The tune rendered to a WAV file (malloc'd, cached); NULL on failure. */
uint8_t *md_tune_render(const uint8_t *d, uint32_t n, uint32_t kind, uint32_t *len);

#endif
