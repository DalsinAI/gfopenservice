/*
 * ACMedia's capability probe (design 3.3): which hardware decoders this host
 * has, for which codecs and sizes. V4L2 memory-to-memory decoders (the Pi 5's
 * rpi-hevc-dec, the Pi 4's rpivid and bcm2835-codec) by their formats; VAAPI
 * (x86-64) by its profiles. The self-test clips (a decode checked against
 * software) are run by the harness (tests/acm_test.c, "caps").
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <libavutil/hwcontext.h>
#if defined(__x86_64__) && !defined(ACM_NO_VAAPI_CAPS)
#include <va/va.h>
#include <libavutil/hwcontext_vaapi.h>
#endif
#include "acm_codecs.h"
#include "acm_caps.h"

#ifndef V4L2_PIX_FMT_HEVC_SLICE
#define V4L2_PIX_FMT_HEVC_SLICE v4l2_fourcc('S', '2', '6', '5')
#endif

static uint32_t v4l2_tag(uint32_t f, int *stateless)
{
    *stateless = 0;
    switch (f) {
    case V4L2_PIX_FMT_HEVC_SLICE: *stateless = 1; return ACM_T_HEVC;
    case V4L2_PIX_FMT_H264_SLICE: *stateless = 1; return ACM_T_H264;
    case V4L2_PIX_FMT_HEVC: return ACM_T_HEVC;
    case V4L2_PIX_FMT_H264: return ACM_T_H264;
    case V4L2_PIX_FMT_MPEG2: return ACM_T_MPEG2;
    case V4L2_PIX_FMT_VP8: return ACM_T_VP8;
    case V4L2_PIX_FMT_VP9: return ACM_T_VP9;
    default: return 0;
    }
}

/* The video device nodes, listed once before the lock-down: Landlock then
 * lets the worker open them, but not list /dev. */
static char devs[64][32];
static int ndevs = -1;
static void list_devs(void)
{
    if (ndevs >= 0) return;
    ndevs = 0;
    glob_t g;
    if (glob("/dev/video*", 0, NULL, &g)) return;
    for (size_t k = 0; k < g.gl_pathc && ndevs < 64; k++) snprintf(devs[ndevs++], sizeof devs[0], "%s", g.gl_pathv[k]);
    globfree(&g);
}

/* Calls fn for every coded format of every m2m decoder. */
static void each_v4l2(void (*fn)(void *, const char *, uint32_t, int, uint32_t, uint32_t), void *ctx)
{
    list_devs();
    for (int k = 0; k < ndevs; k++) {
        int fd = open(devs[k], O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap)) { close(fd); continue; }
        uint32_t c = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        int mp = !!(c & V4L2_CAP_VIDEO_M2M_MPLANE);
        if (!(c & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))) { close(fd); continue; }
        enum v4l2_buf_type t = mp ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
        for (unsigned i = 0;; i++) {
            struct v4l2_fmtdesc d = { .index = i, .type = t };
            if (ioctl(fd, VIDIOC_ENUM_FMT, &d)) break;
            if (!(d.flags & V4L2_FMT_FLAG_COMPRESSED) && v4l2_fourcc('S', '2', '6', '5') != d.pixelformat) continue;
            int stateless;
            uint32_t tag = v4l2_tag(d.pixelformat, &stateless);
            if (!tag) continue;
            uint32_t mw = 0, mh = 0;
            struct v4l2_frmsizeenum s = { .index = 0, .pixel_format = d.pixelformat };
            if (!ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &s)) {
                if (s.type == V4L2_FRMSIZE_TYPE_DISCRETE) { mw = s.discrete.width; mh = s.discrete.height; }
                else { mw = s.stepwise.max_width; mh = s.stepwise.max_height; }
            }
            fn(ctx, (const char *)cap.card, tag, stateless, mw, mh);
        }
        close(fd);
    }
}

struct has { int stateless, found; };
static void has_cb(void *c, const char *card, uint32_t tag, int stateless, uint32_t w, uint32_t h)
{
    (void)card; (void)w; (void)h;
    struct has *x = c;
    if (x->stateless ? (stateless && tag == ACM_T_HEVC) : (!stateless && tag == ACM_T_H264)) x->found = 1;
}
int acm_caps_has_v4l2(int stateless)
{
    struct has x = { stateless, 0 };
    each_v4l2(has_cb, &x);
    return x.found;
}

static void add(struct acm_caps *c, uint32_t tag, uint32_t path, uint32_t w, uint32_t h, uint32_t depths, const char *dev)
{
    if (c->n >= ACM_MAX_HWCAPS) return;
    struct acm_hwcap *x = &c->c[c->n++];
    x->tag = tag; x->path = path; x->max_width = w; x->max_height = h; x->depths = depths;
    snprintf(x->device, sizeof x->device, "%s", dev);
}

static void fill_cb(void *ctx, const char *card, uint32_t tag, int stateless, uint32_t w, uint32_t h)
{
    struct acm_caps *c = ctx;
    /* the Pi decoders report no sizes: their documented limits */
    if (!w && tag == ACM_T_HEVC) { w = 4096; h = 2304; }
    if (!w && tag == ACM_T_H264) { w = 1920; h = 1088; }
    add(c, tag, stateless ? ACM_PATH_HW_V4L2REQ : ACM_PATH_HW_V4L2M2M, w, h,
        tag == ACM_T_HEVC ? (1u << 8) | (1u << 10) : (1u << 8), card);
}

void acm_caps_fill(struct acm_caps *c, uint32_t paths, AVBufferRef *vaapi)
{
    memset(c, 0, sizeof *c);
    if (paths & (ACM_PATH_HW_V4L2REQ | ACM_PATH_HW_V4L2M2M)) each_v4l2(fill_cb, c);
#if defined(__x86_64__) && !defined(ACM_NO_VAAPI_CAPS)
    if (vaapi) {
        VADisplay dpy = ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)vaapi->data)->hwctx)->display;
        int n = vaMaxNumProfiles(dpy);
        VAProfile pr[64];
        if (n > 64) n = 64;
        if (vaQueryConfigProfiles(dpy, pr, &n) == VA_STATUS_SUCCESS) {
            for (int i = 0; i < n; i++) {
                VAEntrypoint ep[16];
                int ne = 16, vld = 0;
                if (vaQueryConfigEntrypoints(dpy, pr[i], ep, &ne) != VA_STATUS_SUCCESS) continue;
                for (int k = 0; k < ne; k++) if (ep[k] == VAEntrypointVLD) vld = 1;
                if (!vld) continue;
                uint32_t tag = 0, depths = 1u << 8;
                switch (pr[i]) {
                case VAProfileH264Main: case VAProfileH264High: case VAProfileH264ConstrainedBaseline: tag = ACM_T_H264; break;
                case VAProfileHEVCMain: tag = ACM_T_HEVC; break;
                case VAProfileHEVCMain10: tag = ACM_T_HEVC; depths = 1u << 10; break;
                case VAProfileMPEG2Main: case VAProfileMPEG2Simple: tag = ACM_T_MPEG2; break;
                case VAProfileVC1Main: case VAProfileVC1Simple: case VAProfileVC1Advanced: tag = ACM_T_VC1; break;
                case VAProfileVP9Profile0: tag = ACM_T_VP9; break;
                case VAProfileVP9Profile2: tag = ACM_T_VP9; depths = 1u << 10; break;
                case VAProfileAV1Profile0: tag = ACM_T_AV1; depths = (1u << 8) | (1u << 10); break;
                default: break;
                }
                if (!tag) continue;
                uint32_t mw = 0, mh = 0;
                VAConfigID cfg;
                if (vaCreateConfig(dpy, pr[i], VAEntrypointVLD, NULL, 0, &cfg) == VA_STATUS_SUCCESS) {
                    VASurfaceAttrib at[32];
                    unsigned na = 32;
                    if (vaQuerySurfaceAttributes(dpy, cfg, at, &na) == VA_STATUS_SUCCESS)
                        for (unsigned k = 0; k < na; k++) {
                            if (at[k].type == VASurfaceAttribMaxWidth) mw = (uint32_t)at[k].value.value.i;
                            if (at[k].type == VASurfaceAttribMaxHeight) mh = (uint32_t)at[k].value.value.i;
                        }
                    vaDestroyConfig(dpy, cfg);
                }
                int merged = 0;
                for (uint32_t k = 0; k < c->n; k++)
                    if (c->c[k].tag == tag && c->c[k].path == ACM_PATH_HW_VAAPI) { c->c[k].depths |= depths; merged = 1; }
                if (!merged) add(c, tag, ACM_PATH_HW_VAAPI, mw, mh, depths, "vaapi");
            }
        }
        snprintf(c->note, sizeof c->note, "VAAPI: %s", vaQueryVendorString(dpy));
    }
#else
    (void)vaapi;
#endif
}
