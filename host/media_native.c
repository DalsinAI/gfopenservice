/*
 * NDECODE (media_native.h): the Amiga's own picture decoders on the host.
 *
 * Request: arg the frame (GIF), buf0 the file, extra[0] the decoder,
 * extra[1] its mode; buf1 (out) the pixels, buf2 (out) 16 bytes of info:
 * width, height, flags (bit 0 alpha), and one more: a GIF's LUT8 frame's
 * pixels left without a colour, or a WebP animation's first frame's place
 * (x << 16 | y). Answer: result width, aux height of the pixels written
 * (a WebP animation's first frame's own; else the picture's).
 *
 * Pixels: ARGB as A, R, G, B bytes (the 68k's 0xAARRGGBB words as they lie
 * in its memory), or one byte a pixel for a GIF's LUT8 frame; width x
 * height, no padding. A file the decoder refuses answers BADREQUEST, as it
 * fails on the 68k; buf1 too small answers TOOSMALL.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "media_native.h"

#ifndef MD_NATIVE

int md_native_ready(void)
{
    return 0;
}

int md_native_decode(uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4], uint32_t *result, uint32_t *aux)
{
    (void)arg; (void)extra; (void)buf; (void)result; (void)aux;
    return MD_BADREQUEST;
}

#else

#include <stdlib.h>
#include <string.h>

#include "pngdecode.h"
#include "tgadecode.h"
#include "gifdecode.h"
#include "webp/decode.h"
#include "webp/demux.h"

int md_native_ready(void)
{
    return 1;
}

/* The 68k's words as its memory holds them: big-endian. */
static void words_out(const uint32_t *w, uint32_t n, uint8_t *out)
{
    uint32_t i;
    for (i = 0; i < n; i++, out += 4)
        md_put32(out, w[i]);
}

static int info(struct md_buffer *b, uint32_t w, uint32_t h, uint32_t flags, uint32_t more)
{
    if (!b->out || b->length < MD_NATIVE_INFO)
        return MD_TOOSMALL;
    md_put32(b->out, w);
    md_put32(b->out + 4, h);
    md_put32(b->out + 8, flags);
    md_put32(b->out + 12, more);
    b->written = MD_NATIVE_INFO;
    return MD_OK;
}

/* gfopenpng.datatype's loadPNG. */
static int png(const uint8_t *d, uint32_t n, struct md_buffer *px, struct md_buffer *in, uint32_t *w, uint32_t *h)
{
    struct png_head *hd = malloc(sizeof *hd);
    uint8_t *idat = NULL, *raw = NULL;
    uint32_t *argb = NULL;
    void *work = NULL;
    int st = MD_HOSTERROR;

    if (!hd)
        return MD_HOSTERROR;
    if (png_read_head(d, n, hd)) {
        st = MD_BADREQUEST;
        goto out;
    }
    *w = hd->width;
    *h = hd->height;
    if (!px->out || px->length / 4 < (uint64_t)hd->width * hd->height) {
        st = MD_TOOSMALL;
        goto out;
    }
    if (!(idat = malloc(hd->idat_len ? hd->idat_len : 1)) || !(raw = malloc(hd->raw_len ? hd->raw_len : 1))
        || !(argb = malloc((size_t)hd->width * hd->height * 4)) || !(work = malloc(PNG_WORKSIZE)))
        goto out;
    png_gather(d, n, idat);
    if (png_decode(hd, idat, raw, (ULONG *)argb, work)) {
        st = MD_BADREQUEST;
        goto out;
    }
    words_out(argb, hd->width * hd->height, px->out);
    px->written = hd->width * hd->height * 4;
    st = info(in, hd->width, hd->height, hd->alpha ? MD_NATIVE_ALPHA : 0, 0);
out:
    free(work);
    free(argb);
    free(raw);
    free(idat);
    free(hd);
    return st;
}

/* gfopentga.datatype's loadTGA. */
static int tga(const uint8_t *d, uint32_t n, struct md_buffer *px, struct md_buffer *in, uint32_t *w, uint32_t *h)
{
    struct tga_head hd;
    uint32_t *argb = NULL, *pal = NULL;
    UBYTE alpha = 0;
    int st = MD_HOSTERROR;

    if (tga_read_head(d, n, &hd))
        return MD_BADREQUEST;
    *w = hd.width;
    *h = hd.height;
    if (!px->out || px->length / 4 < (uint64_t)hd.width * hd.height)
        return MD_TOOSMALL;
    if (!(argb = malloc((size_t)hd.width * hd.height * 4 + 4))
        || (hd.type == 1 && !(pal = malloc((size_t)hd.map_len * 4 + 4))))
        goto out;
    if (tga_decode(&hd, d, n, (ULONG *)argb, (ULONG *)pal, &alpha)) {
        st = MD_BADREQUEST;
        goto out;
    }
    words_out(argb, hd.width * hd.height, px->out);
    px->written = hd.width * hd.height * 4;
    st = info(in, hd.width, hd.height, alpha ? MD_NATIVE_ALPHA : 0, 0);
out:
    free(pal);
    free(argb);
    return st;
}

/* gfopengif.datatype's loadGIF: frame 0's colour indexes (writeLUT8), or
 * the screen after frame `which` (gif_show from an empty screen). */
static int gif(const uint8_t *d, uint32_t n, uint32_t mode, uint32_t which, struct md_buffer *px,
               struct md_buffer *in, uint32_t *w, uint32_t *h)
{
    struct gif_info g;
    struct gif_frame *frames = NULL;
    struct gif_canvas c;
    uint32_t pixels;
    LONG count;
    int st = MD_HOSTERROR;

    memset(&c, 0, sizeof c);
    if ((count = gif_scan(d, n, &g, NULL, 0)) < 1)
        return MD_BADREQUEST;
    *w = g.width;
    *h = g.height;
    pixels = g.width * g.height;
    if (!px->out || px->length < (uint64_t)pixels * (mode == MD_NATIVE_GIF_LUT8 ? 1 : 4))
        return MD_TOOSMALL;
    if (!(frames = malloc(sizeof *frames * (size_t)count)) || !(c.work = malloc(GIF_WORKSIZE)))
        goto out;
    gif_scan(d, n, &g, frames, (ULONG)count);
    if (mode == MD_NATIVE_GIF_LUT8) {
        const struct gif_frame *f = &frames[0];
        ULONG holes = 0;
        if (!f->colours) {
            st = MD_BADREQUEST;
            goto out;
        }
        gif_frame_lut8(d, n, &g, f, px->out, c.work, f->transparent ? f->trans : 0, &holes);
        px->written = pixels;
        st = info(in, g.width, g.height, 0, holes);
        goto out;
    }
    if (which >= g.frames)
        which = g.frames - 1;
    if (!(c.argb = malloc((size_t)pixels * 4 + 4)) || (g.restores && !(c.saved = malloc((size_t)pixels * 4 + 4))))
        goto out;
    c.shown = -1;
    gif_show(d, n, &g, frames, &c, which);
    words_out(c.argb, pixels, px->out);
    px->written = pixels * 4;
    st = info(in, g.width, g.height, gif_has_alpha(&g, &c) ? MD_NATIVE_ALPHA : 0, 0);
out:
    free(c.saved);
    free(c.argb);
    free(c.work);
    free(frames);
    return st;
}

/* webp.datatype's loadWebP: WebPDecodeARGB of the file, or of an
 * animation's first frame (writeFirstFrame). w x h is the canvas; the
 * pixels are the frame's (info[3] its place). */
static int webp(const uint8_t *d, uint32_t n, struct md_buffer *px, struct md_buffer *in, uint32_t *w, uint32_t *h)
{
    WebPBitstreamFeatures f;
    uint8_t *argb;
    int fw = 0, fh = 0, st;
    uint32_t at = 0;

    if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK || f.width <= 0 || f.height <= 0)
        return MD_BADREQUEST;
    *w = (uint32_t)f.width;
    *h = (uint32_t)f.height;
    if (!px->out || px->length / 4 < (uint64_t)f.width * f.height)
        return MD_TOOSMALL;
    if (f.has_animation) {
        WebPData data = { d, n };
        WebPDemuxer *demux = WebPDemux(&data);
        WebPIterator it;
        argb = NULL;
        if (!demux)
            return MD_BADREQUEST;
        if (WebPDemuxGetFrame(demux, 1, &it)) {
            argb = WebPDecodeARGB(it.fragment.bytes, it.fragment.size, &fw, &fh);
            at = (uint32_t)it.x_offset << 16 | (uint32_t)it.y_offset;
            WebPDemuxReleaseIterator(&it);
        }
        WebPDemuxDelete(demux);
    } else
        argb = WebPDecodeARGB(d, n, &fw, &fh);
    if (!argb)
        return MD_BADREQUEST;
    if ((uint64_t)fw * fh * 4 > px->length) {
        WebPFree(argb);
        return MD_TOOSMALL;
    }
    memcpy(px->out, argb, (size_t)fw * fh * 4);
    px->written = (uint32_t)fw * (uint32_t)fh * 4;
    WebPFree(argb);
    *w = (uint32_t)fw;                 /* the pixels' own size; the canvas is in the info */
    *h = (uint32_t)fh;
    st = info(in, (uint32_t)f.width, (uint32_t)f.height, f.has_alpha ? MD_NATIVE_ALPHA : 0, at);
    return st;
}

int md_native_decode(uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4], uint32_t *result, uint32_t *aux)
{
    const uint8_t *d = buf[0].in;
    uint32_t n = buf[0].length, w = 0, h = 0;
    int st;

    if (!d || !n)
        return MD_BADREQUEST;
    if (!buf[2].out || buf[2].length < MD_NATIVE_INFO)
        return MD_TOOSMALL;
    switch (extra[0]) {
    case MD_NATIVE_PNG: st = png(d, n, &buf[1], &buf[2], &w, &h); break;
    case MD_NATIVE_TGA: st = tga(d, n, &buf[1], &buf[2], &w, &h); break;
    case MD_NATIVE_GIF:
        if (extra[1] > MD_NATIVE_GIF_LUT8)
            return MD_BADREQUEST;
        st = gif(d, n, extra[1], arg, &buf[1], &buf[2], &w, &h);
        break;
    case MD_NATIVE_WEBP: st = webp(d, n, &buf[1], &buf[2], &w, &h); break;
    default: return MD_BADREQUEST;
    }
    /* TOOSMALL still says the size wanted. */
    *result = w;
    *aux = h;
    if (st == MD_TOOSMALL)
        buf[1].written = 0;
    return st;
}

#endif
