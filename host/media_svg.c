/*
 * media.decode/1, SVG: drawn on the host by librsvg and cairo straight at the
 * size the Amiga wants, so an icon costs milliseconds rather than a browser
 * engine's seconds. librsvg is opened with dlopen when first needed: no
 * headers or new packages to build, and a host without it falls back to the
 * rest of the picture chain. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "media_svg.h"

#define SVG_MAX 4096

/* librsvg's, cairo's and GLib's own types, as far as this file needs them. */
typedef struct { double length; int unit; } rsvg_length;
typedef struct { double x, y, width, height; } rsvg_rect;
typedef struct { unsigned domain; int code; char *message; } g_error;
enum { UNIT_PERCENT, UNIT_PX, UNIT_EM, UNIT_EX, UNIT_IN, UNIT_CM, UNIT_MM, UNIT_PT, UNIT_PC };

static struct {
    void *(*new_from_data)(const uint8_t *, unsigned long, g_error **);
    void (*set_dpi)(void *, double);
    void (*dimensions)(void *, int *, rsvg_length *, int *, rsvg_length *, int *, rsvg_rect *);
    int (*render)(void *, void *, const rsvg_rect *, g_error **);
    void (*unref)(void *);
    void (*error_free)(g_error *);
    void *(*surface_create)(int, int, int);
    void *(*create)(void *);
    void (*scale)(void *, double, double);
    void (*translate)(void *, double, double);
    void (*flush)(void *);
    uint8_t *(*data)(void *);
    int (*stride)(void *);
    void (*destroy)(void *);
    void (*surface_destroy)(void *);
} rs;
static int have_rsvg;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void open_rsvg(void)
{
    void *h = dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_LOCAL);
#define SYM(field, name) (*(void **)&rs.field = dlsym(h, name))
    if (!h)
        return;
    have_rsvg = SYM(new_from_data, "rsvg_handle_new_from_data") && SYM(set_dpi, "rsvg_handle_set_dpi")
                && SYM(dimensions, "rsvg_handle_get_intrinsic_dimensions")
                && SYM(render, "rsvg_handle_render_document") && SYM(unref, "g_object_unref")
                && SYM(error_free, "g_error_free") && SYM(surface_create, "cairo_image_surface_create")
                && SYM(create, "cairo_create") && SYM(scale, "cairo_scale") && SYM(translate, "cairo_translate")
                && SYM(flush, "cairo_surface_flush") && SYM(data, "cairo_image_surface_get_data")
                && SYM(stride, "cairo_image_surface_get_stride") && SYM(destroy, "cairo_destroy")
                && SYM(surface_destroy, "cairo_surface_destroy");
#undef SYM
}

int md_svg_is(const uint8_t *d, uint32_t n, const char *hint)
{
    uint32_t i, end = n < 4096 ? n : 4096;

    if (!d || n < 5)
        return 0;
    pthread_once(&once, open_rsvg);
    if (!have_rsvg)
        return 0;
    /* SVGZ: gzip, named as SVG. */
    if (d[0] == 0x1f && d[1] == 0x8b)
        return !strcmp(hint, "svg") || !strcmp(hint, "svgz");
    /* Text that has an <svg element near the start. */
    for (i = 0; i < end; i++)
        if (!d[i])
            return 0;
    for (i = 0; i + 4 <= end; i++)
        if (d[i] == '<' && !memcmp(d + i + 1, "svg", 3)
            && (i + 4 == end || d[i + 4] == ' ' || d[i + 4] == '>' || d[i + 4] == '\t' || d[i + 4] == '\n'
                || d[i + 4] == '\r' || d[i + 4] == ':'))
            return 1;
    return !strcmp(hint, "svg") && d[0] == '<';
}

/* A length in pixels at 96 dots an inch; 0 for a percentage. */
static double px(const rsvg_length *l)
{
    static const double per[] = { 0, 1, 16, 8, 96, 96 / 2.54, 96 / 25.4, 96.0 / 72, 16 };
    return l->unit > UNIT_PERCENT && l->unit <= UNIT_PC && l->length > 0 ? l->length * per[l->unit] : 0;
}

static uint32_t side(double v)
{
    return v < 1 ? 1 : v > 65535 ? 65535 : (uint32_t)lround(v);
}

int md_svg_call(uint16_t op, const uint32_t extra[4], struct md_buffer buf[4], uint32_t *result, uint32_t *aux)
{
    g_error *err = NULL;
    void *handle, *surface = NULL, *cr = NULL;
    int has_w = 0, has_h = 0, has_vb = 0, st = MD_OK;
    rsvg_length lw, lh;
    rsvg_rect vb, port;
    double w, h;
    uint32_t iw, ih, ow, oh, x, y;

    if (!buf[1].out || (op == MD_PROBE && buf[1].length < MD_INFO_SIZE))
        return MD_TOOSMALL;
    if (!(handle = rs.new_from_data(buf[0].in, buf[0].length, &err))) {
        if (err)
            rs.error_free(err);
        return MD_BADREQUEST;
    }
    rs.set_dpi(handle, 96);

    /* Its own size, as a browser takes it: width and height, else the
     * viewBox's, else 300 x 150. One of width and height goes with the
     * viewBox's aspect. */
    memset(&vb, 0, sizeof vb);
    rs.dimensions(handle, &has_w, &lw, &has_h, &lh, &has_vb, &vb);
    has_vb = has_vb && vb.width > 0 && vb.height > 0;
    w = has_w ? px(&lw) : 0;
    h = has_h ? px(&lh) : 0;
    if (has_vb && !w && !h) {
        w = vb.width;
        h = vb.height;
    } else if (has_vb && !h)
        h = w * vb.height / vb.width;
    else if (has_vb && !w)
        w = h * vb.width / vb.height;
    if (!w)
        w = 300;
    if (!h)
        h = 150;
    iw = side(w);
    ih = side(h);

    /* The size it is drawn at. */
    if (extra[3] & MD_EXACT) {
        ow = extra[0] ? extra[0] : extra[1] ? side((double)extra[1] * w / h) : iw;
        oh = extra[1] ? extra[1] : extra[0] ? side((double)extra[0] * h / w) : ih;
    } else
        md_fit(iw, ih, extra[0], extra[1], &ow, &oh);
    md_fit(ow, oh, SVG_MAX, SVG_MAX, &ow, &oh);

    if (op == MD_PROBE) {
        md_put32(buf[1].out, MD_KIND_PICTURE);
        md_put32(buf[1].out + 4, MD_FORMAT_SVG);
        md_put32(buf[1].out + 8, MD_FLAG_ALPHA);
        md_put32(buf[1].out + 12, 1);
        md_put32(buf[1].out + 16, ow);
        md_put32(buf[1].out + 20, oh);
        buf[1].written = MD_INFO_SIZE;
        *result = iw;
        *aux = ih;
        goto out;
    }
    if ((uint64_t)ow * oh * 4 > buf[1].length) {
        st = MD_TOOSMALL;
        goto out;
    }

    /* Drawn into an ow x oh viewport, which places it by preserveAspectRatio.
     * Without a viewBox nothing would scale, so it is scaled as a whole and
     * centred, as browsers do for a picture. */
    surface = rs.surface_create(0 /* ARGB32 */, (int)ow, (int)oh);
    cr = rs.create(surface);
    port.x = port.y = 0;
    port.width = ow;
    port.height = oh;
    if (!has_vb && (ow != iw || oh != ih)) {
        double s = fmin(ow / w, oh / h);
        rs.translate(cr, (ow - w * s) / 2, (oh - h * s) / 2);
        rs.scale(cr, s, s);
        port.width = w;
        port.height = h;
    }
    if (!rs.render(handle, cr, &port, &err)) {
        if (err)
            rs.error_free(err);
        st = MD_BADREQUEST;
        goto out;
    }
    rs.flush(surface);

    /* cairo's premultiplied native-endian words to straight A,R,G,B bytes. */
    {
        const uint8_t *row = rs.data(surface);
        int stride = rs.stride(surface);
        uint8_t *o = buf[1].out;
        for (y = 0; y < oh; y++, row += stride) {
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < ow; x++, o += 4) {
                uint32_t v = p[x], a = v >> 24, r = v >> 16 & 0xff, g = v >> 8 & 0xff, b = v & 0xff;
                if (a && a < 255) {
                    r = (r * 255 + a / 2) / a;
                    g = (g * 255 + a / 2) / a;
                    b = (b * 255 + a / 2) / a;
                }
                o[0] = (uint8_t)a;
                o[1] = (uint8_t)(r > 255 ? 255 : r);
                o[2] = (uint8_t)(g > 255 ? 255 : g);
                o[3] = (uint8_t)(b > 255 ? 255 : b);
            }
        }
    }
    buf[1].written = ow * oh * 4;
    *result = ow;
    *aux = oh;
out:
    if (cr)
        rs.destroy(cr);
    if (surface)
        rs.surface_destroy(surface);
    rs.unref(handle);
    return st;
}
