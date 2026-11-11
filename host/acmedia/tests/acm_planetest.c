/*
 * acm_planetest: ACMedia milestone M4K, step 2 (amicradle-teams
 * designs/ACMEDIA-DESIGN-20261110.md, section 9). It decodes a clip in
 * acmedia-worker on the hardware path and puts each frame's dma-buf straight
 * on a KMS overlay plane: no acscreen, no Amiga, no CPU copy. That proves the
 * zero-copy route (SAND128 on the Pi's HVS, NV12 elsewhere), the flip timing,
 * and the frame hand-back to the worker, before acscreen grows a video layer.
 *
 *   acm_planetest WORKER CLIP [--card /dev/dri/cardN] [--frames N] [--fps F]
 *                 [--test-only] [--no-pace] [--plane ID] [--verbose]
 *
 *   --test-only  import every frame and check it with a TEST_ONLY atomic
 *                commit, never flip: for a box whose screen another program
 *                owns, or that has no screen (it still needs a CRTC to test
 *                against, so a connected connector or an active CRTC)
 *   --no-pace    flip as fast as frames come (the plane's top rate)
 *   --fps F      pace at F frames a second (default: the stream's rate)
 *
 * It needs DRM master on the card (nothing else showing on it: stop acscreen
 * first, in a lab), a connected display unless --test-only, and a worker
 * whose hardware decoder hands out dma-bufs (ACM_PATH_HW_*). The last line is
 * "planetest: PASS ..." or "planetest: FAIL ...", for scripts.
 *
 * Team 6, 11 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "../src/acm_broker.h"

#define MAXF 16384
static uint8_t buf[ACM_MSG_MAX];
static int verbose;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sleep_until(double t) {
    struct timespec ts; ts.tv_sec = (time_t)t; ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* ---- KMS ---- */
struct kms {
    int fd;
    uint32_t conn, crtc, crtc_index, primary, overlay;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    uint32_t dumb_handle, dumb_fb;           /* a black primary plane, so the CRTC has something to scan */
    int modeset_done;
};
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name) {
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; p && i < p->count_props && !id; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, name)) id = r->prop_id;
        drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return id;
}
static int prop_value(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *v) {
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    int found = 0;
    for (uint32_t i = 0; p && i < p->count_props && !found; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, name)) { *v = p->prop_values[i]; found = 1; }
        drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return found;
}
/* Does the plane take fourcc with modifier? Read from its IN_FORMATS blob. */
static int plane_takes(int fd, uint32_t plane, uint32_t fourcc, uint64_t mod) {
    uint64_t blob_id;
    if (!prop_value(fd, plane, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id) || !blob_id) {
        drmModePlane *p = drmModeGetPlane(fd, plane); int ok = 0;   /* no modifiers: the plain format list, linear only */
        for (uint32_t i = 0; p && i < p->count_formats; i++) if (p->formats[i] == fourcc) ok = mod == DRM_FORMAT_MOD_LINEAR;
        drmModeFreePlane(p); return ok;
    }
    drmModePropertyBlobRes *b = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
    if (!b) return 0;
    drmModeFormatModifierIterator it = { 0 }; int ok = 0;
    while (!ok && drmModeFormatModifierBlobIterNext(b, &it)) if (it.fmt == fourcc && it.mod == mod) ok = 1;
    drmModeFreePropertyBlob(b);
    return ok;
}
static int kms_open(struct kms *k, const char *card, int test_only) {
    memset(k, 0, sizeof *k);
    char path[64];
    for (int n = card ? 0 : 0; n < 8; n++) {
        if (card) snprintf(path, sizeof path, "%s", card); else snprintf(path, sizeof path, "/dev/dri/card%d", n);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            drmModeRes *r = drmModeGetResources(fd);
            if (r && r->count_crtcs && r->count_connectors) {
                drmModeFreeResources(r);
                k->fd = fd;
                break;
            }
            drmModeFreeResources(r); close(fd);
        }
        if (card) break;
    }
    if (!k->fd) { fprintf(stderr, "planetest: no KMS card (%s)\n", card ? card : "/dev/dri/card0..7"); return -1; }
    if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) || drmSetClientCap(k->fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "planetest: the card has no atomic modesetting\n"); return -1;
    }
    drmModeRes *r = drmModeGetResources(k->fd);
    /* a connected connector, its CRTC (the one in use, else the first its encoders can drive) and its mode */
    for (int i = 0; i < r->count_connectors && !k->conn; i++) {
        drmModeConnector *c = drmModeGetConnector(k->fd, r->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
            uint32_t crtc = 0;
            drmModeEncoder *e = c->encoder_id ? drmModeGetEncoder(k->fd, c->encoder_id) : NULL;
            if (e && e->crtc_id) crtc = e->crtc_id;
            drmModeFreeEncoder(e);
            for (int j = 0; j < c->count_encoders && !crtc; j++) {
                drmModeEncoder *e2 = drmModeGetEncoder(k->fd, c->encoders[j]);
                for (int m = 0; e2 && m < r->count_crtcs && !crtc; m++) if (e2->possible_crtcs & (1u << m)) crtc = r->crtcs[m];
                drmModeFreeEncoder(e2);
            }
            if (crtc) {
                k->conn = c->connector_id; k->crtc = crtc;
                drmModeCrtc *cur = drmModeGetCrtc(k->fd, crtc);
                if (cur && cur->mode_valid) k->mode = cur->mode;          /* keep the mode the screen is in */
                else { k->mode = c->modes[0]; for (int m = 0; m < c->count_modes; m++) if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { k->mode = c->modes[m]; break; } }
                drmModeFreeCrtc(cur);
            }
        }
        drmModeFreeConnector(c);
    }
    if (!k->conn) { fprintf(stderr, "planetest: no connected display%s\n", test_only ? " (--test-only still needs a CRTC to test against)" : ""); drmModeFreeResources(r); return -1; }
    for (int i = 0; i < r->count_crtcs; i++) if (r->crtcs[i] == k->crtc) k->crtc_index = (uint32_t)i;
    drmModeFreeResources(r);
    if (drmModeCreatePropertyBlob(k->fd, &k->mode, sizeof k->mode, &k->mode_blob)) { fprintf(stderr, "planetest: mode blob: %s\n", strerror(errno)); return -1; }
    /* the primary plane of that CRTC */
    drmModePlaneRes *pr = drmModeGetPlaneResources(k->fd);
    for (uint32_t i = 0; pr && i < pr->count_planes && !k->primary; i++) {
        drmModePlane *p = drmModeGetPlane(k->fd, pr->planes[i]); uint64_t t;
        if (p && (p->possible_crtcs & (1u << k->crtc_index)) && prop_value(k->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &t) && t == DRM_PLANE_TYPE_PRIMARY) k->primary = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    /* a black XRGB8888 dumb buffer for it */
    struct drm_mode_create_dumb cd = { .width = k->mode.hdisplay, .height = k->mode.vdisplay, .bpp = 32 };
    if (drmIoctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { fprintf(stderr, "planetest: dumb buffer: %s\n", strerror(errno)); return -1; }
    k->dumb_handle = cd.handle;
    uint32_t h[4] = { cd.handle }, pi[4] = { cd.pitch }, off[4] = { 0 };
    if (drmModeAddFB2(k->fd, cd.width, cd.height, DRM_FORMAT_XRGB8888, h, pi, off, &k->dumb_fb, 0)) { fprintf(stderr, "planetest: primary fb: %s\n", strerror(errno)); return -1; }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (!drmIoctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        void *m = mmap(NULL, cd.size, PROT_WRITE, MAP_SHARED, k->fd, (off_t)md.offset);
        if (m != MAP_FAILED) { memset(m, 0, cd.size); munmap(m, cd.size); }
    }
    printf("planetest: card fd %d, connector %u, crtc %u, mode %ux%u@%u, primary plane %u\n",
           k->fd, k->conn, k->crtc, k->mode.hdisplay, k->mode.vdisplay, k->mode.vrefresh, k->primary);
    return 0;
}
/* An overlay plane on our CRTC that takes the frames' format (or the one asked for). */
static int kms_pick_overlay(struct kms *k, uint32_t fourcc, uint64_t mod, uint32_t want) {
    drmModePlaneRes *pr = drmModeGetPlaneResources(k->fd);
    for (uint32_t i = 0; pr && i < pr->count_planes && !k->overlay; i++) {
        drmModePlane *p = drmModeGetPlane(k->fd, pr->planes[i]); uint64_t t;
        if (p && (!want || p->plane_id == want) && (p->possible_crtcs & (1u << k->crtc_index)) &&
            prop_value(k->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &t) && t == DRM_PLANE_TYPE_OVERLAY &&
            plane_takes(k->fd, p->plane_id, fourcc, mod)) k->overlay = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (!k->overlay) fprintf(stderr, "planetest: no overlay plane on crtc %u takes %.4s modifier 0x%016llx\n", k->crtc, (char *)&fourcc, (unsigned long long)mod);
    return k->overlay ? 0 : -1;
}

/* ---- a frame on the plane ---- */
struct shown { uint32_t slot, fb, handles[4], nh; double due; int live; };
static int import_frame(struct kms *k, const struct acm_frame *fr, const int *fds, int nfds, struct shown *s) {
    memset(s, 0, sizeof *s);
    uint32_t objh[4] = { 0 };
    for (int i = 0; i < nfds && i < 4; i++) {
        if (drmPrimeFDToHandle(k->fd, fds[i], &objh[i])) { fprintf(stderr, "planetest: PRIME import: %s\n", strerror(errno)); return -1; }
        s->handles[s->nh++] = objh[i];
    }
    uint32_t h[4] = { 0 }, pitch[4] = { 0 }, off[4] = { 0 }; uint64_t mods[4] = { 0 };
    for (uint32_t p = 0; p < fr->nplanes && p < 4; p++) {
        if (fr->obj[p] >= (uint32_t)nfds) return -1;
        h[p] = objh[fr->obj[p]]; pitch[p] = fr->pitch[p]; off[p] = fr->offset[p]; mods[p] = fr->drm_modifier;
    }
    uint32_t fourcc = fr->drm_fourcc ? fr->drm_fourcc : fr->pixfmt == ACM_PIX_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
    if (drmModeAddFB2WithModifiers(k->fd, fr->width, fr->height, fourcc, h, pitch, off, mods, &s->fb, DRM_MODE_FB_MODIFIERS)) {
        fprintf(stderr, "planetest: AddFB2 %ux%u %.4s mod 0x%016llx: %s\n", fr->width, fr->height, (char *)&fourcc, (unsigned long long)fr->drm_modifier, strerror(errno));
        return -1;
    }
    s->slot = fr->slot; s->live = 1;
    return 0;
}
static void drop_frame(struct kms *k, struct acm_worker *w, struct shown *s) {
    if (!s->live) return;
    if (s->fb) drmModeRmFB(k->fd, s->fb);
    for (uint32_t i = 0; i < s->nh; i++) {           /* one handle per distinct object (PRIME gives the same one twice) */
        int dup = 0; for (uint32_t j = 0; j < i; j++) if (s->handles[j] == s->handles[i]) dup = 1;
        if (!dup) { struct drm_gem_close gc = { .handle = s->handles[i] }; drmIoctl(k->fd, DRM_IOCTL_GEM_CLOSE, &gc); }
    }
    uint32_t rt, len;
    acm_ask(w, ACM_Q_RELEASE, &(struct acm_release){ s->slot, 0 }, sizeof(struct acm_release), NULL, 0, &rt, buf, sizeof buf, &len, NULL, NULL, 2000);
    s->live = 0;
}
/* The overlay's destination: the frame scaled to fit the mode, aspect kept, centred */
static void fit(const struct kms *k, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y, uint32_t *dw, uint32_t *dh) {
    uint64_t mw = k->mode.hdisplay, mh = k->mode.vdisplay;
    if (mw * h <= mh * w) { *dw = (uint32_t)mw; *dh = (uint32_t)(mw * h / w); } else { *dh = (uint32_t)mh; *dw = (uint32_t)(mh * w / h); }
    *x = (uint32_t)((mw - *dw) / 2); *y = (uint32_t)((mh - *dh) / 2);
}
#define ADD(obj, type, name, val) do { uint32_t _p = prop_id(k->fd, obj, type, name); \
    if (!_p) { fprintf(stderr, "planetest: no property %s\n", name); drmModeAtomicFree(req); return -1; } \
    if (drmModeAtomicAddProperty(req, obj, _p, val) < 0) { drmModeAtomicFree(req); return -1; } } while (0)
/* Show fb (the frame w x h) on the overlay. flags: TEST_ONLY, or a flip with an event. */
static int commit(struct kms *k, uint32_t fb, uint32_t w, uint32_t h, uint32_t flags) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!k->modeset_done) {
        ADD(k->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", k->crtc);
        ADD(k->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", k->mode_blob);
        ADD(k->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
        if (k->primary) {
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "FB_ID", k->dumb_fb);
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "CRTC_ID", k->crtc);
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "SRC_X", 0); ADD(k->primary, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)k->mode.hdisplay << 16);
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)k->mode.vdisplay << 16);
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0); ADD(k->primary, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
            ADD(k->primary, DRM_MODE_OBJECT_PLANE, "CRTC_W", k->mode.hdisplay); ADD(k->primary, DRM_MODE_OBJECT_PLANE, "CRTC_H", k->mode.vdisplay);
        }
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    uint32_t x, y, dw, dh; fit(k, w, h, &x, &y, &dw, &dh);
    uint32_t o = k->overlay;
    ADD(o, DRM_MODE_OBJECT_PLANE, "FB_ID", fb);
    ADD(o, DRM_MODE_OBJECT_PLANE, "CRTC_ID", k->crtc);
    ADD(o, DRM_MODE_OBJECT_PLANE, "SRC_X", 0); ADD(o, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
    ADD(o, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)w << 16); ADD(o, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)h << 16);
    ADD(o, DRM_MODE_OBJECT_PLANE, "CRTC_X", x); ADD(o, DRM_MODE_OBJECT_PLANE, "CRTC_Y", y);
    ADD(o, DRM_MODE_OBJECT_PLANE, "CRTC_W", dw); ADD(o, DRM_MODE_OBJECT_PLANE, "CRTC_H", dh);
    int r = drmModeAtomicCommit(k->fd, req, flags, k);
    drmModeAtomicFree(req);
    if (r) fprintf(stderr, "planetest: atomic commit%s: %s\n", (flags & DRM_MODE_ATOMIC_TEST_ONLY) ? " (test)" : "", strerror(errno));
    else if (!(flags & DRM_MODE_ATOMIC_TEST_ONLY)) k->modeset_done = 1;
    return r;
}
static double flip_at;
static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data) {
    (void)fd; (void)seq; (void)crtc; (void)data; flip_at = sec + usec / 1e6;
}
static int wait_flip(struct kms *k, int timeout_ms) {
    struct pollfd p = { k->fd, POLLIN, 0 };
    drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
    flip_at = 0;
    while (!flip_at) {
        int r = poll(&p, 1, timeout_ms);
        if (r <= 0) return -1;
        if (drmHandleEvent(k->fd, &ev)) return -1;
    }
    return 0;
}
static double proc_cpu(pid_t pid) {                  /* seconds of CPU a process has used */
    char path[64], line[1024]; snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    double s = 0;
    if (fgets(line, sizeof line, f)) {
        char *p = strrchr(line, ')'); unsigned long ut = 0, st = 0;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) s = (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
    }
    fclose(f); return s;
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: acm_planetest WORKER CLIP [--card PATH] [--frames N] [--fps F] [--test-only] [--no-pace] [--plane ID] [--verbose]\n"); return 2; }
    const char *exe = argv[1], *clip = argv[2], *card = NULL;
    int frames = 300, test_only = 0, pace = 1; double fps = 0; uint32_t want_plane = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--plane") && i + 1 < argc) want_plane = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--test-only")) test_only = 1;
        else if (!strcmp(argv[i], "--no-pace")) pace = 0;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else { fprintf(stderr, "planetest: unknown %s\n", argv[i]); return 2; }
    }
    if (frames < 1 || frames > MAXF) frames = MAXF;
    struct kms k;
    if (kms_open(&k, card, test_only)) { printf("planetest: FAIL no KMS output\n"); return 1; }

    struct acm_worker w; struct acm_hello h; acm_hello_all(&h, ACM_HF_HW);
    int r = acm_spawn(&w, exe, 1, 4096, !verbose, &h);
    if (r) { printf("planetest: FAIL worker: %s\n", w.why); return 1; }
    printf("planetest: worker %d, sandbox 0x%x, hardware paths 0x%x\n", (int)w.pid, w.ready.sandbox, w.ready.hw);
    int src = open(clip, O_RDONLY | O_CLOEXEC);
    if (src < 0 || acm_give_source(&w, src)) { printf("planetest: FAIL source %s\n", clip); acm_end(&w); return 1; }
    uint32_t t, len;
    struct acm_open o = { -1, -2, 0, 0 };
    r = acm_ask(&w, ACM_Q_OPEN, &o, sizeof o, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 15000);
    if (r || t != ACM_A_OPENED) { printf("planetest: FAIL open (%d)\n", r ? r : acm_err(t, buf, ACM_E_BADARG)); acm_end(&w); return 1; }
    struct acm_opened op; memcpy(&op, buf, sizeof op);
    printf("planetest: %s, %ux%u, path 0x%x\n", clip, op.width, op.height, op.vpath);
    if (!(op.vpath & (ACM_PATH_HW_VAAPI | ACM_PATH_HW_V4L2REQ | ACM_PATH_HW_V4L2M2M)))
        { printf("planetest: FAIL the stream is not on a hardware decoder (path 0x%x)\n", op.vpath); acm_end(&w); return 1; }
    if (op.nslots && acm_give_pool(&w, op.slot_size, op.nslots)) { printf("planetest: FAIL pool\n"); acm_end(&w); return 1; }
    if (fps <= 0 && op.video >= 0 && (uint32_t)op.video < op.info.nstreams && op.info.s[op.video].rate_den)
        fps = (double)op.info.s[op.video].rate_num / op.info.s[op.video].rate_den;
    if (fps <= 0) fps = 30;

    static double late[MAXF];
    int shown_n = 0, dropped = 0, notdma = 0, nlate = 0, swframes = 0, pathchg = 0, failed = 0;
    struct shown cur = { 0 }, next = { 0 };
    double t0 = 0, cpu0 = proc_cpu(getpid()), wcpu0 = proc_cpu(w.pid), wall0 = now_s();
    int64_t pts0 = 0;
    for (int n = 0; n < frames && !failed; n++) {
        int fds[ACM_MAX_FDS], nf = 0;
        r = acm_ask(&w, ACM_Q_NEXT, &(struct acm_next){ ACM_WANT_VIDEO | ACM_WANT_DMABUF, 0 }, sizeof(struct acm_next), NULL, 0,
                    &t, buf, sizeof buf, &len, fds, &nf, 5000);
        if (r) { printf("planetest: worker gone at frame %d: %s\n", n, w.why); failed = 1; break; }
        if (t != ACM_A_FRAME) { int e = acm_err(t, buf, ACM_E_BADARG); for (int i = 0; i < nf; i++) close(fds[i]); if (e == ACM_E_EOF) break; printf("planetest: frame %d: error %d\n", n, e); failed = 1; break; }
        struct acm_frame fr; memcpy(&fr, buf, sizeof fr);
        if (fr.flags & ACM_FF_PATH) pathchg++;
        if (!(fr.flags & ACM_FF_HW)) swframes++;
        if (!(fr.flags & ACM_FF_DMABUF)) {            /* a pool frame (software): not for this test; hand it back */
            notdma++;
            for (int i = 0; i < nf; i++) close(fds[i]);
            uint32_t rt; acm_ask(&w, ACM_Q_RELEASE, &(struct acm_release){ fr.slot, 0 }, sizeof(struct acm_release), NULL, 0, &rt, buf, sizeof buf, &len, NULL, NULL, 2000);
            continue;
        }
        if (n == 0) {
            uint32_t fourcc = fr.drm_fourcc ? fr.drm_fourcc : DRM_FORMAT_NV12;
            printf("planetest: frames are %.4s, modifier 0x%016llx, %u object(s), %u plane(s)\n", (char *)&fourcc, (unsigned long long)fr.drm_modifier, fr.nobj, fr.nplanes);
            if (kms_pick_overlay(&k, fourcc, fr.drm_modifier, want_plane)) { for (int i = 0; i < nf; i++) close(fds[i]); failed = 1; break; }
            printf("planetest: overlay plane %u\n", k.overlay);
        }
        int imp = import_frame(&k, &fr, fds, nf, &next);
        for (int i = 0; i < nf; i++) close(fds[i]);   /* the GEM handles keep the buffers */
        if (imp) { next.slot = fr.slot; next.live = 1; drop_frame(&k, &w, &next); failed = 1; break; }
        if (test_only) {
            if (commit(&k, next.fb, fr.width, fr.height, DRM_MODE_ATOMIC_TEST_ONLY)) failed = 1;
            drop_frame(&k, &w, &next); shown_n++;
            continue;
        }
        if (!t0) { t0 = now_s() + 0.05; pts0 = fr.pts_ms; }
        double due = pace ? t0 + (fr.pts_ms != INT64_MIN && fr.pts_ms >= pts0 ? (fr.pts_ms - pts0) / 1000.0 : shown_n / fps) : 0;
        if (pace && now_s() > due + 1.0 / fps && shown_n) { drop_frame(&k, &w, &next); dropped++; continue; }   /* too late for its slot */
        if (pace) sleep_until(due - 0.5 / (k.mode.vrefresh ? k.mode.vrefresh : 60));   /* commit half a refresh early: it flips on the vblank */
        if (commit(&k, next.fb, fr.width, fr.height, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT)) { drop_frame(&k, &w, &next); failed = 1; break; }
        if (wait_flip(&k, 1000)) { printf("planetest: no flip event within 1 s\n"); drop_frame(&k, &w, &next); failed = 1; break; }
        if (pace && nlate < MAXF) late[nlate++] = (now_s() - due) * 1000.0;
        drop_frame(&k, &w, &cur);                     /* the frame before is off the screen: back to the worker */
        cur = next; memset(&next, 0, sizeof next);
        shown_n++;
        if (verbose && shown_n % 30 == 0) printf("planetest: %d shown, %d dropped\n", shown_n, dropped);
    }
    double wall = now_s() - wall0, cpu = proc_cpu(getpid()) - cpu0, wcpu = proc_cpu(w.pid) - wcpu0;
    drop_frame(&k, &w, &cur);
    acm_end(&w);
    double p50 = 0, p99 = 0;
    if (nlate) { qsort(late, (size_t)nlate, sizeof late[0], cmpd); p50 = late[nlate / 2]; p99 = late[(int)(nlate * 0.99) < nlate ? (int)(nlate * 0.99) : nlate - 1]; }
    printf("planetest: %d frames %s, %d dropped, %d not dma-buf, %d software, %d path changes; %.2f s, %.1f fps\n",
           shown_n, test_only ? "tested" : "shown", dropped, notdma, swframes, pathchg, wall, wall > 0 ? shown_n / wall : 0);
    printf("planetest: CPU: this %.1f%% of a core, worker %.1f%%; lateness after due p50 %.2f ms, p99 %.2f ms\n",
           wall > 0 ? 100 * cpu / wall : 0, wall > 0 ? 100 * wcpu / wall : 0, p50, p99);
    int pass = !failed && shown_n > 0 && !notdma && !swframes;
    printf("planetest: %s\n", pass ? "PASS zero-copy frames on the overlay plane" : "FAIL (see above)");
    return pass ? 0 : 1;
}
