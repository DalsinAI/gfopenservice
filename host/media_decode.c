/*
 * media.decode/1 on the host (docs/MEDIA_DECODE.md): AVIF through libavif,
 * HEIC/HEIF through libheif, camera RAW and anything else ImageMagick reads
 * through those tools (media_tool.c), answered as 32-bit ARGB scaled to fit;
 * with MD_AV, JPEG, PNG, GIF, WebP, JPEG XL, PSD, EXR... and sounds, videos,
 * MIDI and SID tunes through FFmpeg (media_av.c).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "media_decode.h"

#include <stdlib.h>
#include <string.h>

#include <avif/avif.h>
#include <libheif/heif.h>

#include "media_tool.h"
#include "media_svg.h"
#ifdef MD_AV
#include "media_av.h"
#endif

/* Larger pictures are refused: 2^28 pixels is 1 GB of ARGB. */
#define MAX_PIXELS (1u << 28)

struct picture {
    uint32_t format, flags, frames;
    uint32_t width, height;
    uint8_t *argb;             /* width x height x 4, malloc'd; NULL after a probe */
};

void md_put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static int brand_is(const uint8_t *b, const char *const *list)
{
    for (; *list; list++)
        if (!memcmp(b, *list, 4))
            return 1;
    return 0;
}

/* The format from the ftyp box: its major brand, then its compatible brands. */
static uint32_t sniff(const uint8_t *d, uint32_t n)
{
    static const char *const avif[] = { "avif", "avis", NULL };
    static const char *const heic[] = { "heic", "heix", "hevc", "hevx", "heim", "heis", "hevm", "hevs",
                                        "mif1", "msf1", NULL };
    uint32_t box, i;

    if (n < 16 || memcmp(d + 4, "ftyp", 4))
        return 0;
    box = (uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
    if (box < 16 || box > n)
        box = n < 64 ? n : 64;
    if (brand_is(d + 8, avif))
        return MD_FORMAT_AVIF;
    if (brand_is(d + 8, heic) && memcmp(d + 8, "mif1", 4) && memcmp(d + 8, "msf1", 4))
        return MD_FORMAT_HEIC;
    for (i = 16; i + 4 <= box; i += 4)       /* mif1 files say what they hold here */
        if (brand_is(d + i, avif))
            return MD_FORMAT_AVIF;
    for (i = 16; i + 4 <= box; i += 4)
        if (brand_is(d + i, heic))
            return MD_FORMAT_HEIC;
    return brand_is(d + 8, heic) ? MD_FORMAT_HEIC : 0;
}

/* ---- AVIF -------------------------------------------------------------- */

static int avif_load(const uint8_t *d, uint32_t n, uint32_t frame, int decode, struct picture *p)
{
    avifDecoder *dec = avifDecoderCreate();
    avifRGBImage rgb;
    int st = MD_BADREQUEST;

    if (!dec)
        return MD_HOSTERROR;
    dec->maxThreads = 4;
    if (avifDecoderSetIOMemory(dec, d, n) != AVIF_RESULT_OK || avifDecoderParse(dec) != AVIF_RESULT_OK)
        goto out;
    p->format = MD_FORMAT_AVIF;
    p->flags = dec->alphaPresent ? MD_FLAG_ALPHA : 0;
    p->frames = dec->imageCount > 0 ? (uint32_t)dec->imageCount : 1;
    p->width = dec->image->width;
    p->height = dec->image->height;
    if (!decode) {
        st = MD_OK;
        goto out;
    }
    if (frame >= p->frames || avifDecoderNthImage(dec, frame) != AVIF_RESULT_OK)
        goto out;
    p->width = dec->image->width;
    p->height = dec->image->height;
    if ((uint64_t)p->width * p->height > MAX_PIXELS)
        goto out;
    avifRGBImageSetDefaults(&rgb, dec->image);
    rgb.format = AVIF_RGB_FORMAT_ARGB;
    rgb.depth = 8;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
        st = MD_HOSTERROR;
        goto out;
    }
    if (avifImageYUVToRGB(dec->image, &rgb) == AVIF_RESULT_OK && (p->argb = malloc((size_t)p->width * p->height * 4))) {
        uint32_t y;
        for (y = 0; y < p->height; y++)
            memcpy(p->argb + (size_t)y * p->width * 4, rgb.pixels + (size_t)y * rgb.rowBytes, (size_t)p->width * 4);
        st = MD_OK;
    }
    avifRGBImageFreePixels(&rgb);
out:
    avifDecoderDestroy(dec);
    return st;
}

/* ---- HEIC / HEIF ----------------------------------------------------- */

static int heif_load(const uint8_t *d, uint32_t n, uint32_t frame, int decode, struct picture *p)
{
    struct heif_context *ctx = heif_context_alloc();
    struct heif_image_handle *h = NULL;
    struct heif_image *img = NULL;
    struct heif_error e;
    int st = MD_BADREQUEST, count;

    if (!ctx)
        return MD_HOSTERROR;
    e = heif_context_read_from_memory_without_copy(ctx, d, n, NULL);
    if (e.code != heif_error_Ok)
        goto out;
    count = heif_context_get_number_of_top_level_images(ctx);
    if (count < 1)
        goto out;
    if (frame == 0)
        e = heif_context_get_primary_image_handle(ctx, &h);
    else {
        heif_item_id *ids = calloc(count, sizeof *ids);
        if (!ids || frame >= (uint32_t)count) {
            free(ids);
            goto out;
        }
        heif_context_get_list_of_top_level_image_IDs(ctx, ids, count);
        e = heif_context_get_image_handle(ctx, ids[frame], &h);
        free(ids);
    }
    if (e.code != heif_error_Ok)
        goto out;
    p->format = MD_FORMAT_HEIC;
    p->flags = heif_image_handle_has_alpha_channel(h) ? MD_FLAG_ALPHA : 0;
    p->frames = count;
    p->width = heif_image_handle_get_width(h);
    p->height = heif_image_handle_get_height(h);
    if (!decode) {
        st = MD_OK;
        goto out;
    }
    if ((uint64_t)p->width * p->height > MAX_PIXELS)
        goto out;
    e = heif_decode_image(h, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL);
    if (e.code == heif_error_Ok) {
        int stride = 0;
        const uint8_t *src = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
        uint32_t w = heif_image_get_width(img, heif_channel_interleaved);
        uint32_t hh = heif_image_get_height(img, heif_channel_interleaved);
        if (src && w && hh && (uint64_t)w * hh <= MAX_PIXELS && (p->argb = malloc((size_t)w * hh * 4))) {
            uint32_t x, y;
            for (y = 0; y < hh; y++) {
                const uint8_t *s = src + (size_t)y * stride;
                uint8_t *o = p->argb + (size_t)y * w * 4;
                for (x = 0; x < w; x++, s += 4, o += 4) {
                    o[0] = s[3]; o[1] = s[0]; o[2] = s[1]; o[3] = s[2];
                }
            }
            p->width = w;
            p->height = hh;
            st = MD_OK;
        }
    }
out:
    if (img)
        heif_image_release(img);
    if (h)
        heif_image_handle_release(h);
    heif_context_free(ctx);
    return st;
}

/* ---- scaling ----------------------------------------------------------- */

void md_fit(uint32_t w, uint32_t h, uint32_t maxw, uint32_t maxh, uint32_t *ow, uint32_t *oh)
{
    *ow = w;
    *oh = h;
    if (maxw && *ow > maxw) {
        *oh = (uint32_t)((uint64_t)*oh * maxw / *ow);
        *ow = maxw;
    }
    if (maxh && *oh > maxh) {
        *ow = (uint32_t)((uint64_t)*ow * maxh / *oh);
        *oh = maxh;
    }
    if (!*ow)
        *ow = 1;
    if (!*oh)
        *oh = 1;
}

/* Each output pixel is the average of the source pixels its area covers. */
static void shrink(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t ow, uint32_t oh)
{
    uint32_t x, y;

    for (y = 0; y < oh; y++) {
        uint32_t y0 = (uint32_t)((uint64_t)y * h / oh), y1 = (uint32_t)((uint64_t)(y + 1) * h / oh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (x = 0; x < ow; x++) {
            uint32_t x0 = (uint32_t)((uint64_t)x * w / ow), x1 = (uint32_t)((uint64_t)(x + 1) * w / ow);
            uint64_t sum[4] = { 0, 0, 0, 0 }, cnt = 0;
            uint32_t sx, sy;
            if (x1 <= x0)
                x1 = x0 + 1;
            for (sy = y0; sy < y1; sy++) {
                const uint8_t *s = src + ((size_t)sy * w + x0) * 4;
                for (sx = x0; sx < x1; sx++, s += 4) {
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
                }
                cnt += x1 - x0;
            }
            dst[0] = (uint8_t)((sum[0] + cnt / 2) / cnt);
            dst[1] = (uint8_t)((sum[1] + cnt / 2) / cnt);
            dst[2] = (uint8_t)((sum[2] + cnt / 2) / cnt);
            dst[3] = (uint8_t)((sum[3] + cnt / 2) / cnt);
            dst += 4;
        }
    }
}

/* ---- the service ------------------------------------------------------- */

static int load(const struct md_buffer *file, const char *hint, uint32_t frame, int decode, struct picture *p)
{
    uint8_t **argb = decode ? &p->argb : NULL;
    int raw;

    memset(p, 0, sizeof *p);
    if (!file->in || !file->length)
        return MD_BADREQUEST;
    switch (sniff(file->in, file->length)) {
    case MD_FORMAT_AVIF:
        return avif_load(file->in, file->length, frame, decode, p);
    case MD_FORMAT_HEIC:
        return heif_load(file->in, file->length, frame, decode, p);
    }
    p->frames = 1;
    /* A picture inside a ZIP: OpenRaster, Krita, a comic book. */
    if (file->length >= 4 && !memcmp(file->in, "PK\3\4", 4)) {
        struct md_buffer inner = { 0 };
        uint32_t format = 0, len = 0;
        uint8_t *pic = md_zip_picture(file->in, file->length, hint, &format, &len);
        int st;
        if (!pic)
            return MD_BADREQUEST;
        inner.in = pic;
        inner.length = len;
        st = load(&inner, "", frame, decode, p);
        free(pic);
        if (st == MD_OK)
            p->format = format;
        return st;
    }
    /* Camera RAW is TIFF inside, so it goes to LibRaw before FFmpeg sees it. */
    if ((raw = md_is_raw(file->in, file->length, hint)))
        return md_tool_picture(file->in, file->length, hint, 1, &p->format, &p->flags, &p->width, &p->height, argb);
#ifdef MD_AV
    if (md_is_av(file->in, file->length) == MD_AV_STILL
        && md_still_load(file->in, file->length, decode, &p->format, &p->flags, &p->width, &p->height,
                         &p->argb) == MD_OK)
        return MD_OK;
#endif
    return md_tool_picture(file->in, file->length, hint, 0, &p->format, &p->flags, &p->width, &p->height, argb);
}

#ifdef MD_AV
/* A MIDI or SID tune: rendered to WAV on the host, then answered as any sound. */
static int tune(uint16_t op, uint32_t kind, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
                uint32_t *result, uint32_t *aux)
{
    struct md_buffer wav = { 0 };
    uint8_t *d;
    uint32_t len = 0;
    int st;

    if (!buf[1].out || (op == MD_PROBE && buf[1].length < MD_INFO_SIZE))
        return MD_TOOSMALL;
    if (!(d = md_tune_render(buf[0].in, buf[0].length, kind, &len)))
        return MD_BADREQUEST;
    wav.in = d;
    wav.length = len;
    if (op == MD_DECODE)
        st = md_sound_decode(&wav, arg, extra, &buf[1], result, aux);
    else if ((st = md_sound_probe(&wav, extra, buf[1].out, result, aux)) == MD_OK) {
        md_put32(buf[1].out + 4, kind);
        buf[1].written = MD_INFO_SIZE;
    }
    free(d);
    return st;
}
#endif

int md_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux)
{
    struct picture p;
    uint32_t ow, oh;
    char hint[8];
    int st;

    *result = *aux = 0;
    md_hint(extra[2], hint);
    /* SVG is drawn at the size asked, not decoded and shrunk. */
    if ((op == MD_PROBE || op == MD_DECODE) && md_svg_is(buf[0].in, buf[0].length, hint))
        return md_svg_call(op, extra, buf, result, aux);
#ifdef MD_AV
    if (op == MD_VOPEN || op == MD_VOPENURL) {
        if (op == MD_VOPEN && buf[0].length && md_is_av(buf[0].in, buf[0].length) == MD_AV_NONE)
            return MD_BADREQUEST;
        if (op == MD_VOPEN && !buf[0].length && !extra[2])
            return MD_BADREQUEST;
        if (op == MD_VOPENURL && (!buf[0].in || !buf[0].length))
            return MD_BADREQUEST;
        if (!buf[1].out || buf[1].length < MD_INFO_SIZE)
            return MD_TOOSMALL;
        st = op == MD_VOPEN ? md_video_open(&buf[0], extra, buf[1].out, result, aux)
                            : md_video_open_url(&buf[0], extra, buf[1].out, result, aux);
        if (st == MD_OK)
            buf[1].written = MD_INFO_SIZE;
        return st;
    }
    if (op == MD_VSOUND)
        return md_video_sound(arg, extra, &buf[1], result, aux);
    if (op == MD_VUPLOAD)
        return md_video_upload(arg, &buf[0], extra, result, aux);
    if (op == MD_VFRAME)
        return md_video_frame(arg, extra, &buf[1], result, aux);
    if (op == MD_VCLOSE)
        return md_video_close(arg);
    if ((op == MD_PROBE || op == MD_DECODE) && buf[0].in && buf[0].length) {
        uint32_t kind = md_tune_sniff(buf[0].in, buf[0].length);
        if (kind)
            return tune(op, kind, arg, extra, buf, result, aux);
        /* Not a picture: a sound through FFmpeg. */
        if (!sniff(buf[0].in, buf[0].length) && !md_is_raw(buf[0].in, buf[0].length, hint)
            && md_is_av(buf[0].in, buf[0].length) == MD_AV_MEDIA) {
            if (op == MD_DECODE)
                return buf[1].out ? md_sound_decode(&buf[0], arg, extra, &buf[1], result, aux) : MD_TOOSMALL;
            if (!buf[1].out || buf[1].length < MD_INFO_SIZE)
                return MD_TOOSMALL;
            if ((st = md_sound_probe(&buf[0], extra, buf[1].out, result, aux)) == MD_OK)
                buf[1].written = MD_INFO_SIZE;
            return st;
        }
    }
#endif
    switch (op) {
    case MD_PROBE:
        if (!buf[1].out || buf[1].length < MD_INFO_SIZE)
            return MD_TOOSMALL;
        if ((st = load(&buf[0], hint, 0, 0, &p)) != MD_OK)
            return st;
        md_fit(p.width, p.height, extra[0], extra[1], &ow, &oh);
        md_put32(buf[1].out, MD_KIND_PICTURE);
        md_put32(buf[1].out + 4, p.format);
        md_put32(buf[1].out + 8, p.flags);
        md_put32(buf[1].out + 12, p.frames);
        md_put32(buf[1].out + 16, ow);
        md_put32(buf[1].out + 20, oh);
        buf[1].written = MD_INFO_SIZE;
        *result = p.width;
        *aux = p.height;
        return MD_OK;
    case MD_DECODE:
        if (!buf[1].out)
            return MD_TOOSMALL;
        if ((st = load(&buf[0], hint, arg, 1, &p)) != MD_OK)
            return st;
        md_fit(p.width, p.height, extra[0], extra[1], &ow, &oh);
        if ((uint64_t)ow * oh * 4 > buf[1].length) {
            free(p.argb);
            return MD_TOOSMALL;
        }
        if (ow == p.width && oh == p.height)
            memcpy(buf[1].out, p.argb, (size_t)ow * oh * 4);
        else
            shrink(p.argb, p.width, p.height, buf[1].out, ow, oh);
        free(p.argb);
        buf[1].written = ow * oh * 4;
        *result = ow;
        *aux = oh;
        return MD_OK;
    default:
        return MD_BADREQUEST;
    }
}
