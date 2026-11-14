/*
 * acm_test: ACMedia's host test harness (stage a). It plays the broker's part
 * with acm_broker.c and talks to real, sandboxed workers.
 *
 *   acm_test info FILE                         probe
 *   acm_test caps                              the hardware decoders here
 *   acm_test hash FILE [--hw] [--copy] [--n N] a checksum of every decoded frame
 *   acm_test conformance CLIPDIR               the protocol, good and bad requests
 *   acm_test policy FILE                       a deny never opens the decoder
 *   acm_test crash FILE                        crash, hang, leak: the broker survives
 *   acm_test sandbox                           what the sandbox must refuse
 *   acm_test fallback FILE                     hardware fails mid-stream: software goes on, same frames
 *   acm_test fuzz SEEDDIR N                    N corrupted files through probe, open and decode
 *   acm_test bench FILE [--hw] [--copy] [--dmabuf] [--realtime] [--n N]
 *
 * ACM_WORKER and ACM_WORKER_TEST name the two workers (default: beside this
 * program; the test one is built with ACM_TEST_HOOKS). Exit 0 is a pass.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../src/acm_broker.h"
#include "../src/acm_codecs.h"

static char worker[PATH_MAX], worker_test[PATH_MAX];
static uint8_t buf[ACM_MSG_MAX];
static int failures;
static char lasterr[128];   /* the worker's last error text */

#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

struct sess {
    struct acm_worker w;
    struct acm_opened op;
    int fd;
};

/* spawn, source, open, pool. 0 or an ACM_E_ */
static int sess_open(struct sess *s, const char *exe, const char *file, int hw, uint32_t flags, const struct acm_hello *h,
                     int video_only)
{
    memset(s, 0, sizeof *s);
    struct acm_hello hh;
    if (!h) { acm_hello_all(&hh, flags | (hw ? ACM_HF_HW : 0)); h = &hh; }
    /* RLIMIT_AS is a backstop; a hardware worker maps the GPU driver and its surfaces too */
    int r = acm_spawn(&s->w, exe, hw, hw ? 4096 : 1536, !getenv("ACM_VERBOSE"), h);
    if (r) return r;
    s->fd = open(file, O_RDONLY | O_CLOEXEC);
    if (s->fd < 0) { acm_end(&s->w); return ACM_E_IO; }
    r = acm_give_source(&s->w, s->fd);
    if (r) return r;
    struct acm_open o = { video_only == 2 ? -2 : -1, video_only == 1 ? -2 : -1, 0, 0 };
    uint32_t t, len;
    r = acm_ask(&s->w, ACM_Q_OPEN, &o, sizeof o, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 15000);
    if (r) return r;
    if (t != ACM_A_OPENED) {
        if (t == ACM_A_ERROR) snprintf(lasterr, sizeof lasterr, "%s", ((struct acm_error *)buf)->text);
        return acm_err(t, buf, ACM_E_BADARG);
    }
    memcpy(&s->op, buf, sizeof s->op);
    if (s->op.nslots) return acm_give_pool(&s->w, s->op.slot_size, s->op.nslots);
    return 0;
}
static void sess_close(struct sess *s)
{
    acm_end(&s->w);
    if (s->fd > 0) close(s->fd);
    s->fd = -1;
}

/* A frame's checksum, the same for the same pictures whatever the layout:
 * NV12 and P010 (hardware read back) are hashed as planar YUV 4:2:0 and
 * 10-bit planar would be, so hardware and software can be compared. */
static uint64_t hash_frame(const struct sess *s, const struct acm_frame *fr)
{
    uint64_t h = 0xcbf29ce484222325ull;
    if ((fr->flags & ACM_FF_DMABUF) || fr->slot >= s->w.nslots) return h;
    const uint8_t *base = s->w.pool + (size_t)fr->slot * s->w.slot_size;
    static uint8_t row[16384];
    if (fr->pixfmt == ACM_PIX_NV12 || fr->pixfmt == ACM_PIX_P010) {
        int wide = fr->pixfmt == ACM_PIX_P010;
        for (uint32_t y = 0; y < fr->rows[0]; y++) {
            const uint8_t *r = base + fr->offset[0] + (size_t)y * fr->pitch[0];
            if (!wide) h = fnv(h, r, fr->rowbytes[0]);
            else for (uint32_t x = 0; x + 1 < fr->rowbytes[0]; x += 2) {
                uint16_t v = (uint16_t)((r[x] | r[x + 1] << 8) >> 6);
                h = fnv(h, (const uint8_t *)&v, 2);
            }
        }
        for (int c = 0; c < 2; c++)
            for (uint32_t y = 0; y < fr->rows[1]; y++) {
                const uint8_t *r = base + fr->offset[1] + (size_t)y * fr->pitch[1];
                uint32_t n = 0;
                if (!wide) for (uint32_t x = (uint32_t)c; x < fr->rowbytes[1] && n < sizeof row; x += 2) row[n++] = r[x];
                else for (uint32_t x = (uint32_t)c * 2; x + 1 < fr->rowbytes[1] && n + 1 < sizeof row; x += 4) {
                    uint16_t v = (uint16_t)((r[x] | r[x + 1] << 8) >> 6);
                    row[n++] = (uint8_t)v; row[n++] = (uint8_t)(v >> 8);
                }
                h = fnv(h, row, n);
            }
        return h;
    }
    for (uint32_t p = 0; p < fr->nplanes && p < 4; p++)
        for (uint32_t y = 0; y < fr->rows[p]; y++)
            h = fnv(h, base + fr->offset[p] + (size_t)y * fr->pitch[p], fr->rowbytes[p]);
    return h;
}

/* the next video frame; its planes' checksum in *h. 0, or an ACM_E_ */
static int next_frame(struct sess *s, uint32_t want, struct acm_frame *fr, uint64_t *h, int timeout)
{
    uint32_t t, len;
    int fds[ACM_MAX_FDS], nf = 0;
    int r = acm_ask(&s->w, ACM_Q_NEXT, &(struct acm_next){ want, 0 }, sizeof(struct acm_next), NULL, 0,
                    &t, buf, sizeof buf, &len, fds, &nf, timeout);
    if (r) return r;
    if (t != ACM_A_FRAME) {
        for (int i = 0; i < nf; i++) close(fds[i]);
        if (t == ACM_A_ERROR) snprintf(lasterr, sizeof lasterr, "%s", ((struct acm_error *)buf)->text);
        return acm_err(t, buf, ACM_E_BADARG);
    }
    memcpy(fr, buf, sizeof *fr);
    if (h) *h = hash_frame(s, fr);
    for (int i = 0; i < nf; i++) close(fds[i]);
    uint32_t rt;
    r = acm_ask(&s->w, ACM_Q_RELEASE, &(struct acm_release){ fr->slot, 0 }, sizeof(struct acm_release), NULL, 0,
                &rt, buf, sizeof buf, &len, NULL, NULL, 2000);
    if (r) return r;
    return rt == ACM_A_OK ? 0 : acm_err(rt, buf, ACM_E_BADARG);
}

static int next_audio(struct sess *s, int *samples)
{
    uint32_t t, len;
    int r = acm_ask(&s->w, ACM_Q_NEXT, &(struct acm_next){ ACM_WANT_AUDIO, 0 }, sizeof(struct acm_next), NULL, 0,
                    &t, buf, sizeof buf, &len, NULL, NULL, 10000);
    if (r) return r;
    if (t != ACM_A_AUDIO) return acm_err(t, buf, ACM_E_BADARG);
    const struct acm_audio *a = (const void *)buf;
    if (len != sizeof *a + a->frames * a->channels * 2) return ACM_E_BADARG;
    *samples += (int)a->frames;
    return 0;
}

static const char *pathname(uint32_t p)
{
    switch (p) {
    case ACM_PATH_SW: return "software";
    case ACM_PATH_HW_VAAPI: return "hw-vaapi";
    case ACM_PATH_HW_V4L2REQ: return "hw-v4l2-request";
    case ACM_PATH_HW_V4L2M2M: return "hw-v4l2-m2m";
    default: return "none";
    }
}

static void print_info(const struct acm_info *in)
{
    char t[5];
    printf("container %s (%s), %lld ms, %u streams\n", acm_tagstr(in->container, t), in->container_name,
           (long long)in->duration_ms, in->nstreams);
    for (uint32_t i = 0; i < in->nstreams; i++) {
        const struct acm_stream *x = &in->s[i];
        printf("  #%u %s %s", i, x->kind == 1 ? "video" : x->kind == 2 ? "audio" : x->kind == 3 ? "subs" : "other",
               x->tag ? acm_tagstr(x->tag, t) : "----");
        if (x->kind == 1) printf(" %ux%u %u-bit %u/%u fps", x->width, x->height, x->bitdepth, x->rate_num, x->rate_den);
        if (x->kind == 2) printf(" %u Hz %u ch", x->sample_rate, x->channels);
        printf("%s%s%s\n", x->flags & ACM_SF_DENIED ? " DENIED" : "", x->flags & ACM_SF_HWOK ? " hw" : "",
               x->flags & ACM_SF_NOCODEC ? " no-codec" : "");
    }
}

/* ---- info, caps ---- */

static int cmd_info(const char *file)
{
    struct acm_worker w;
    struct acm_hello h;
    acm_hello_all(&h, ACM_HF_HW);
    int r = acm_spawn(&w, worker, 1, 1536, 1, &h);
    if (r) { printf("spawn: %d %s\n", r, w.why); return 1; }
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    r = fd < 0 ? ACM_E_IO : acm_give_source(&w, fd);
    uint32_t t, len;
    if (!r) r = acm_ask(&w, ACM_Q_PROBE, NULL, 0, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 10000);
    if (!r && t == ACM_A_INFO) print_info((const struct acm_info *)buf);
    else printf("probe: %d\n", r ? r : acm_err(t, buf, 0));
    acm_end(&w);
    if (fd >= 0) close(fd);
    return r ? 1 : 0;
}

static int cmd_caps(void)
{
    struct acm_worker w;
    struct acm_hello h;
    acm_hello_all(&h, ACM_HF_HW);
    int r = acm_spawn(&w, worker, 1, 1536, 1, &h);
    if (r) { printf("spawn: %d %s\n", r, w.why); return 1; }
    printf("sandbox %#x (seccomp %d, landlock %d abi %u, nnp %d, rlimits %d); hardware paths %#x\n",
           w.ready.sandbox, !!(w.ready.sandbox & ACM_SB_SECCOMP), !!(w.ready.sandbox & ACM_SB_LANDLOCK),
           w.ready.landlock_abi, !!(w.ready.sandbox & ACM_SB_NNP), !!(w.ready.sandbox & ACM_SB_RLIMIT), w.ready.hw);
    uint32_t t, len;
    r = acm_ask(&w, ACM_Q_CAPS, NULL, 0, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 10000);
    if (!r && t == ACM_A_CAPS) {
        const struct acm_caps *c = (const void *)buf;
        char tg[5];
        for (uint32_t i = 0; i < c->n; i++)
            printf("  %s %-16s up to %ux%u%s%s (%s)\n", acm_tagstr(c->c[i].tag, tg), pathname(c->c[i].path),
                   c->c[i].max_width, c->c[i].max_height, c->c[i].depths & (1u << 8) ? " 8-bit" : "",
                   c->c[i].depths & (1u << 10) ? " 10-bit" : "", c->c[i].device);
        if (c->note[0]) printf("  %s\n", c->note);
        if (!c->n) printf("  no hardware decoders\n");
    }
    acm_end(&w);
    return r ? 1 : 0;
}

/* ---- hash ---- */

static int cmd_hash(const char *file, int hw, int copy, int n, int quiet)
{
    struct sess s;
    int r = sess_open(&s, worker, file, hw, copy ? ACM_HF_COPY : 0, NULL, 1);
    if (r) { printf("open: %d %s\n", r, s.w.why); sess_close(&s); return 1; }
    printf("path %s, %ux%u, pool %u x %u KB\n", pathname(s.op.vpath), s.op.width, s.op.height, s.op.nslots,
           s.op.slot_size >> 10);
    uint64_t all = 0xcbf29ce484222325ull;
    int frames = 0, copied = 0, hwf = 0, changed = 0;
    struct acm_frame fr;
    uint64_t h;
    while (n <= 0 || frames < n) {
        r = next_frame(&s, ACM_WANT_VIDEO, &fr, &h, 10000);
        if (r == ACM_E_EOF) { r = 0; break; }
        if (r) { printf("frame %d: %d %s%s\n", frames, r, s.w.why, lasterr); break; }
        if (!quiet) printf("%5d %8lld %016llx%s%s%s\n", frames, (long long)fr.pts_ms, (unsigned long long)h,
                           fr.flags & ACM_FF_HW ? " hw" : "", fr.flags & ACM_FF_COPIED ? " copied" : "",
                           fr.flags & ACM_FF_PATH ? " path-changed" : "");
        all = fnv(all, (const uint8_t *)&h, sizeof h);
        frames++;
        copied += !!(fr.flags & ACM_FF_COPIED);
        hwf += !!(fr.flags & ACM_FF_HW);
        changed += !!(fr.flags & ACM_FF_PATH);
    }
    printf("frames %d hw %d copied %d path-changes %d all %016llx\n", frames, hwf, copied, changed,
           (unsigned long long)all);
    sess_close(&s);
    return r ? 1 : 0;
}

/* ---- conformance ---- */

static int ask_simple(struct acm_worker *w, uint32_t type, const void *p, uint32_t n, const int *fds, int nfds, int *code)
{
    uint32_t t, len;
    int r = acm_ask(w, type, p, n, fds, nfds, &t, buf, sizeof buf, &len, NULL, NULL, 10000);
    if (r) { *code = r; return (int)ACM_A_ERROR; }
    *code = t == ACM_A_ERROR ? ((struct acm_error *)buf)->code : 0;
    return (int)t;
}

static uint32_t tag_from_name(const char *name)
{
    char t[5] = { 0 };
    int i = 0;
    for (; name[i] && name[i] != '-' && i < 4; i++) t[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    t[i] = 0;
    return acm_tag_parse(t);
}

static int cmd_conformance(const char *dir)
{
    struct acm_worker w;
    struct acm_hello h;
    acm_hello_all(&h, 0);
    int r = acm_spawn(&w, worker, 0, 512, 1, &h), c;
    CHECK(!r, "spawn %d %s", r, w.why);
    if (r) return 1;
    CHECK((w.ready.sandbox & (ACM_SB_SECCOMP | ACM_SB_NNP | ACM_SB_RLIMIT)) == (ACM_SB_SECCOMP | ACM_SB_NNP | ACM_SB_RLIMIT),
          "sandbox bits %#x", w.ready.sandbox);
    CHECK(ask_simple(&w, ACM_Q_HELLO, &h, sizeof h, NULL, 0, &c) == ACM_A_ERROR && c == ACM_E_BADARG, "second HELLO: %d", c);
    CHECK(ask_simple(&w, 999, NULL, 0, NULL, 0, &c) == ACM_A_ERROR && c == ACM_E_BADARG, "unknown request: %d", c);
    CHECK(ask_simple(&w, ACM_Q_NEXT, &(struct acm_next){ ACM_WANT_VIDEO, 0 }, sizeof(struct acm_next), NULL, 0, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "NEXT before OPEN: %d", c);
    CHECK(ask_simple(&w, ACM_Q_OPEN, &(struct acm_open){ -1, -1, 0, 0 }, sizeof(struct acm_open), NULL, 0, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "OPEN before SOURCE: %d", c);
    CHECK(ask_simple(&w, ACM_Q_OPEN, "x", 1, NULL, 0, &c) == ACM_A_ERROR && c == ACM_E_BADARG, "short OPEN: %d", c);
    CHECK(ask_simple(&w, ACM_Q_RELEASE, &(struct acm_release){ 5, 0 }, sizeof(struct acm_release), NULL, 0, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "RELEASE of nothing: %d", c);
    int rw = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int wr = open("/dev/null", O_WRONLY | O_CLOEXEC);
    CHECK(ask_simple(&w, ACM_Q_SOURCE, &(struct acm_source){ 0, 0, 0 }, sizeof(struct acm_source), &wr, 1, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "a writable source refused: %d", c);
    CHECK(ask_simple(&w, ACM_Q_SOURCE, &(struct acm_source){ 0, 0, 0 }, sizeof(struct acm_source), NULL, 0, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "SOURCE without an fd: %d", c);
    /* a program is not media */
    CHECK(ask_simple(&w, ACM_Q_SOURCE, &(struct acm_source){ 0, 0, 0 }, sizeof(struct acm_source), &rw, 1, &c) == ACM_A_OK, "SOURCE: %d", c);
    CHECK(ask_simple(&w, ACM_Q_PROBE, NULL, 0, NULL, 0, &c) == ACM_A_ERROR && c == ACM_E_FORMAT, "probe of an ELF: %d", c);
    CHECK(ask_simple(&w, ACM_Q_SOURCE, &(struct acm_source){ 0, 0, 0 }, sizeof(struct acm_source), &rw, 1, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "a second SOURCE: %d", c);
    CHECK(ask_simple(&w, ACM_Q_POOL, &(struct acm_pool){ 100, 7, 7 }, sizeof(struct acm_pool), &rw, 1, &c) == ACM_A_ERROR
          && c == ACM_E_BADARG, "a bad pool: %d", c);
    close(rw); close(wr);
    CHECK(ask_simple(&w, ACM_Q_CLOSE, NULL, 0, NULL, 0, &c) == ACM_A_OK, "CLOSE");
    acm_end(&w);
    CHECK(w.exit_code == 0 && w.exit_signal == 0, "worker exit %d signal %d", w.exit_code, w.exit_signal);

    /* every clip: probe says its codec; open, the frames, EOF, EOF again, seek, frames again */
    DIR *d = opendir(dir);
    CHECK(d != NULL, "no clips in %s", dir);
    struct dirent *e;
    int clips = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        uint32_t want = tag_from_name(e->d_name);
        if (!want) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct sess s;
        int audio = 0;
        r = sess_open(&s, worker, p, 0, 0, NULL, 1);
        if (r == ACM_E_FORMAT) { sess_close(&s); audio = 1; r = sess_open(&s, worker, p, 0, 0, NULL, 2); }
        CHECK(!r, "%s: open %d %s", e->d_name, r, s.w.why);
        if (r) { sess_close(&s); continue; }
        int ok = 0;
        for (uint32_t i = 0; i < s.op.info.nstreams; i++) if (s.op.info.s[i].tag == want) ok = 1;
        char t1[5];
        CHECK(ok, "%s: no %s stream", e->d_name, acm_tagstr(want, t1));
        if (audio) {
            int n = 0, samples = 0;
            while ((r = next_audio(&s, &samples)) == 0) n++;
            CHECK(r == ACM_E_EOF && n > 0 && samples > 0, "%s: %d audio blocks then %d", e->d_name, n, r);
            CHECK(ask_simple(&s.w, ACM_Q_SEEK, &(struct acm_seek){ 0, 0, 0 }, sizeof(struct acm_seek), NULL, 0, &c) == ACM_A_OK,
                  "%s: seek %d", e->d_name, c);
            int s2 = 0, n2 = 0;
            while (next_audio(&s, &s2) == 0) n2++;
            CHECK(abs(s2 - samples) <= samples / 50, "%s: %d samples after seeking to 0, %d before", e->d_name, s2, samples);   /* Opus and Vorbis pre-roll differs a little after a seek */
            printf("  %-28s %s, %d samples (%u Hz %u ch)\n", e->d_name, acm_tagstr(want, t1), samples,
                   s.op.sample_rate, s.op.channels);
            sess_close(&s);
            clips++;
            continue;
        }
        struct acm_frame fr;
        int n = 0;
        while ((r = next_frame(&s, ACM_WANT_VIDEO, &fr, NULL, 10000)) == 0) n++;
        CHECK(r == ACM_E_EOF && n > 0, "%s: %d frames then %d", e->d_name, n, r);
        CHECK(next_frame(&s, ACM_WANT_VIDEO, &fr, NULL, 10000) == ACM_E_EOF, "%s: EOF twice", e->d_name);
        CHECK(ask_simple(&s.w, ACM_Q_SEEK, &(struct acm_seek){ 0, 0, 0 }, sizeof(struct acm_seek), NULL, 0, &c) == ACM_A_OK,
              "%s: seek %d", e->d_name, c);
        int n2 = 0;
        while (next_frame(&s, ACM_WANT_VIDEO, &fr, NULL, 10000) == 0) n2++;
        CHECK(n2 == n, "%s: %d frames after seeking to 0, %d before", e->d_name, n2, n);
        CHECK(ask_simple(&s.w, ACM_Q_RELEASE, &(struct acm_release){ fr.slot, 0 }, sizeof(struct acm_release), NULL, 0, &c)
              == ACM_A_ERROR && c == ACM_E_BADARG, "%s: a slot released twice: %d", e->d_name, c);
        printf("  %-28s %s, %d frames, path %s\n", e->d_name, acm_tagstr(want, t1), n, pathname(s.op.vpath));
        sess_close(&s);
        clips++;
    }
    if (d) closedir(d);

    /* Asking for audio first must not feed-and-drop the video packets that
     * the shared demuxer crosses on its way to each audio packet. */
    char avp[PATH_MAX];
    snprintf(avp, sizeof avp, "%s/h264-av.mp4", dir);
    struct sess ref, mix;
    struct acm_frame vf;
    int refn = 0, mixn = 0, ablocks = 0, samples = 0;
    r = sess_open(&ref, worker, avp, 0, 0, NULL, 1);
    while (!r && next_frame(&ref, ACM_WANT_VIDEO, &vf, NULL, 10000) == 0) refn++;
    sess_close(&ref);
    r = sess_open(&mix, worker, avp, 0, 0, NULL, 0);
    if (!r) while (next_audio(&mix, &samples) == 0) ablocks++;
    if (!r) while (next_frame(&mix, ACM_WANT_VIDEO, &vf, NULL, 10000) == 0) mixn++;
    CHECK(!r && ablocks > 0 && samples > 0, "h264-av: audio-first decode (%d blocks, %d samples)", ablocks, samples);
    CHECK(refn > 0 && mixn == refn, "h264-av: audio-first preserves every video packet (%d/%d frames)", mixn, refn);
    sess_close(&mix);
    CHECK(clips >= 5, "only %d clips", clips);
    printf("conformance: %d clips, %s\n", clips, failures ? "FAILED" : "pass");
    return failures ? 1 : 0;
}

/* ---- policy ---- */

static int cmd_policy(const char *file)
{
    struct acm_hello h;
    acm_hello_all(&h, 0);
    /* the file's video codec, denied */
    struct sess s;
    int r = sess_open(&s, worker, file, 0, 0, NULL, 1);
    CHECK(!r, "plain open %d", r);
    uint32_t vt = 0;
    for (uint32_t i = 0; i < s.op.info.nstreams; i++) if (s.op.info.s[i].kind == ACM_KIND_VIDEO) { vt = s.op.info.s[i].tag; break; }
    sess_close(&s);
    CHECK(vt != 0, "no video tag");
    uint32_t n = 0;
    for (uint32_t i = 0; i < h.nallow; i++) if (h.allow[i] != vt) h.allow[n++] = h.allow[i];
    h.nallow = n;
    r = sess_open(&s, worker, file, 0, 0, &h, 1);
    CHECK(r == ACM_E_DENIED, "denied codec opened: %d", r);
    CHECK(!s.w.dead || s.w.exit_signal == 0, "the worker died on a deny");
    /* probe still answers, marked */
    uint32_t t, len;
    r = acm_ask(&s.w, ACM_Q_PROBE, NULL, 0, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 5000);
    int marked = 0;
    if (!r && t == ACM_A_INFO) {
        const struct acm_info *in = (const void *)buf;
        for (uint32_t i = 0; i < in->nstreams; i++) if (in->s[i].tag == vt && (in->s[i].flags & ACM_SF_DENIED)) marked = 1;
    }
    CHECK(marked, "probe does not mark the denied stream");
    sess_close(&s);
    /* hardware-only with no hardware: denied too */
    acm_hello_all(&h, 0);
    n = 0;
    for (uint32_t i = 0; i < h.nallow; i++) if (h.allow[i] != vt) h.allow[n++] = h.allow[i];
    h.nallow = n;
    h.hwonly[0] = vt;
    h.nhwonly = 1;
    r = sess_open(&s, worker, file, 0, 0, &h, 1);
    CHECK(r == ACM_E_DENIED, "hw-only codec opened in software: %d", r);
    sess_close(&s);
    printf("policy: %s\n", failures ? "FAILED" : "pass");
    return failures ? 1 : 0;
}

/* ---- crash, hang, leak ---- */

static int hook_on(struct sess *s, uint32_t hk, uint32_t arg)
{
    uint32_t t, len;
    int r = acm_ask(&s->w, ACM_Q_HOOK, &(struct acm_hook){ hk, arg }, sizeof(struct acm_hook), NULL, 0, &t, buf, sizeof buf,
                    &len, NULL, NULL, 2000);
    return r ? r : t == ACM_A_OK ? 0 : acm_err(t, buf, ACM_E_BADARG);
}

static int cmd_crash(const char *file)
{
    static const struct { uint32_t hook; const char *name; } k[] = {
        { ACM_HOOK_CRASH, "crash" }, { ACM_HOOK_HANG, "hang" }, { ACM_HOOK_LEAK, "leak" } };
    for (unsigned i = 0; i < 3; i++) {
        struct sess s;
        int r = sess_open(&s, worker_test, file, 0, 0, NULL, 1);
        CHECK(!r, "%s: open %d %s", k[i].name, r, s.w.why);
        CHECK(!hook_on(&s, k[i].hook, 3), "%s: hook", k[i].name);
        struct acm_frame fr;
        int n = 0;
        double t0 = now();
        while ((r = next_frame(&s, ACM_WANT_VIDEO, &fr, NULL, 2000)) == 0) n++;
        double dt = now() - t0;
        CHECK(r == ACM_E_DECODER, "%s: %d frames then %d (want DECODER)", k[i].name, n, r);
        printf("  %-5s after %d frames: %s, %.2f s; %s\n", k[i].name, n, r == ACM_E_DECODER ? "ACM_E_DECODER" : "?", dt, s.w.why);
        sess_close(&s);
        /* the next playback is fine */
        r = sess_open(&s, worker, file, 0, 0, NULL, 1);
        n = 0;
        while (!r && next_frame(&s, ACM_WANT_VIDEO, &fr, NULL, 5000) == 0) n++;
        CHECK(!r && n > 3, "%s: the next session played %d frames (%d)", k[i].name, n, r);
        sess_close(&s);
    }
    printf("crash: %s\n", failures ? "FAILED" : "pass");
    return failures ? 1 : 0;
}

/* ---- sandbox ---- */

static int cmd_sandbox(int hw)
{
    static const char *what[] = { "files", "socket()", "execve()", "fork()", "mmap(PROT_EXEC)", "kill(broker)" };
    for (uint32_t a = 0; a < 6; a++) {
        struct acm_worker w;
        struct acm_hello h;
        acm_hello_all(&h, hw ? ACM_HF_HW : 0);
        int r = acm_spawn(&w, worker_test, hw, 512, 1, &h);
        CHECK(!r, "spawn %d %s", r, w.why);
        if (r) continue;
        uint32_t t, len;
        r = acm_ask(&w, ACM_Q_HOOK, &(struct acm_hook){ ACM_HOOK_ESCAPE, a }, sizeof(struct acm_hook), NULL, 0,
                    &t, buf, sizeof buf, &len, NULL, NULL, 3000);
        if (a == 0) {
            uint32_t worked = !r && t == ACM_A_ERROR ? ((struct acm_error *)buf)->detail : 0xffffffffu;
            CHECK(worked == 0, "files: these worked: %#x (1 /etc/passwd, 2 write /tmp, 4 list /home)", worked);
            printf("  %-16s refused (open /etc/passwd, write /tmp, list /home)%s\n", what[a],
                   w.ready.sandbox & ACM_SB_LANDLOCK ? "" : " [no Landlock!]");
        } else {
            CHECK(r == ACM_E_DECODER && w.exit_signal == SIGSYS, "%s: not stopped by seccomp (r %d, signal %d)", what[a], r, w.exit_signal);
            printf("  %-16s %s\n", what[a], w.exit_signal == SIGSYS ? "killed by seccomp" : "NOT STOPPED");
        }
        acm_end(&w);
    }
    unlink("/tmp/acm-escape-test");
    printf("sandbox: %s\n", failures ? "FAILED" : "pass");
    return failures ? 1 : 0;
}

/* ---- fallback ---- */

#define MAXF 4096
static int cmd_fallback(const char *file)
{
    static int64_t pts[MAXF];
    static uint64_t hs[MAXF];
    struct sess s;
    struct acm_frame fr;
    uint64_t h;
    int n = 0, r = sess_open(&s, worker, file, 0, 0, NULL, 1);
    CHECK(!r, "software open %d", r);
    while (n < MAXF && next_frame(&s, ACM_WANT_VIDEO, &fr, &h, 10000) == 0) { pts[n] = fr.pts_ms; hs[n++] = h; }
    sess_close(&s);
    r = sess_open(&s, worker_test, file, 1, 0, NULL, 1);
    CHECK(!r, "hardware open %d %s", r, s.w.why);
    if (r || s.op.vpath == ACM_PATH_SW) {
        printf("fallback: no hardware decoder takes this file here (path %s): skipped\n", pathname(s.op.vpath));
        sess_close(&s);
        return failures ? 1 : 0;
    }
    CHECK(!hook_on(&s, ACM_HOOK_FAIL_HW, 10), "hook");
    int got = 0, changes = 0, hwn = 0, swn = 0, mism = 0, i = 0;
    while (next_frame(&s, ACM_WANT_VIDEO, &fr, &h, 10000) == 0) {
        got++;
        changes += !!(fr.flags & ACM_FF_PATH);
        if (fr.flags & ACM_FF_HW) hwn++; else swn++;
        while (i < n && pts[i] < fr.pts_ms) i++;
        if (i >= n || pts[i] != fr.pts_ms || hs[i] != h) mism++;
    }
    CHECK(changes == 1, "%d path changes", changes);
    CHECK(got == n, "%d frames with the fallback, %d in software alone", got, n);
    CHECK(hwn >= 10 && swn > 0, "hw %d sw %d", hwn, swn);
    CHECK(mism == 0, "%d frames differ from software's", mism);
    printf("fallback: %d frames, %d in hardware then %d in software, %d differ from software alone: %s\n",
           got, hwn, swn, mism, failures ? "FAILED" : "pass");
    sess_close(&s);
    return failures ? 1 : 0;
}

/* ---- fuzz ---- */

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static int cmd_fuzz(const char *dir, int iters)
{
    char *seeds[64];
    size_t sizes[64];
    int ns = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && ns < 64) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (32 << 20)) { fclose(f); continue; }
        seeds[ns] = malloc((size_t)sz);
        if (fread(seeds[ns], 1, (size_t)sz, f) == (size_t)sz) sizes[ns++] = (size_t)sz;
        fclose(f);
    }
    if (d) closedir(d);
    CHECK(ns > 0, "no seeds");
    int out_ok = 0, out_err = 0, out_dead = 0, out_sigsys = 0, codes[16] = { 0 };
    double t0 = now();
    uint8_t *m = malloc(32 << 20);
    for (int it = 0; it < iters && ns; it++) {
        int k = (int)(rnd() % (uint32_t)ns);
        size_t n = sizes[k];
        memcpy(m, seeds[k], n);
        switch (rnd() % 4) {
        case 0: for (uint32_t j = 1 + rnd() % 64; j--; ) m[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: n = 1 + rnd() % n; break;
        case 2: { size_t a = rnd() % n, l = rnd() % 4096; if (a + l > n) l = n - a; memset(m + a, (int)(rnd() & 0xff), l); } break;
        case 3: for (uint32_t j = 1 + rnd() % 8; j--; ) { size_t a = rnd() % n; m[a] = (uint8_t)rnd(); m[(a * 7) % n] = 0xff; } break;
        }
        int mfd = memfd_create("acm-fuzz", MFD_CLOEXEC);
        if (mfd < 0 || write(mfd, m, n) != (ssize_t)n) { if (mfd >= 0) close(mfd); continue; }
        char p[64];
        snprintf(p, sizeof p, "/proc/self/fd/%d", mfd);
        struct sess s;
        int r = sess_open(&s, worker, p, 0, 0, NULL, 0);
        close(mfd);
        struct acm_frame fr;
        int smp = 0;
        for (int f = 0; !r && f < 20; f++)
            r = s.op.video >= 0 ? next_frame(&s, ACM_WANT_VIDEO, &fr, NULL, 3000) : next_audio(&s, &smp);
        if (r == ACM_E_EOF || r == 0) out_ok++;
        else if (r == ACM_E_DECODER && s.w.dead) {
            out_dead++;
            if (s.w.exit_signal == SIGSYS) out_sigsys++;
            printf("  run %d (seed %d, mutation kept as fuzz-%d.bin when ACM_FUZZ_KEEP is set): %s %s\n", it, k, it, s.w.why, lasterr);
            if (getenv("ACM_FUZZ_KEEP")) {
                char kp[PATH_MAX];
                snprintf(kp, sizeof kp, "%s/fuzz-%d.bin", getenv("ACM_FUZZ_KEEP"), it);
                FILE *kf = fopen(kp, "wb");
                if (kf) { fwrite(m, 1, n, kf); fclose(kf); }
            }
        } else { out_err++; if (-r < 16) codes[-r]++; if (r == ACM_E_BADARG && getenv("ACM_FUZZ_VERBOSE")) printf("  run %d badarg: %s\n", it, lasterr); }
        sess_close(&s);
    }
    free(m);
    double dt = now() - t0;
    printf("fuzz: %d runs in %.1f s: %d played or ended, %d clean errors (format %d, nocodec %d, io %d, decoder %d), "
           "%d workers died (of them %d by seccomp); the harness lived\n", iters, dt, out_ok, out_err,
           codes[-ACM_E_FORMAT], codes[-ACM_E_NOCODEC], codes[-ACM_E_IO], codes[-ACM_E_DECODER], out_dead, out_sigsys);
    CHECK(out_sigsys == 0, "seccomp killed %d workers on corrupt files", out_sigsys);
    return failures ? 1 : 0;
}

/* ---- bench ---- */

static double proc_cpu(pid_t pid)
{
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *q = strrchr(b, ')');
    if (!q) return 0;
    unsigned long ut = 0, st = 0;
    /* fields after the name: state(3) ... utime(14) stime(15) */
    if (sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return 0;
    return (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
}

static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int cmd_bench(const char *file, int hw, int copy, int dmabuf, int realtime, int n)
{
    struct sess s;
    int r = sess_open(&s, worker, file, hw, copy ? ACM_HF_COPY : 0, NULL, 1);
    if (r) { printf("open: %d %s\n", r, s.w.why); sess_close(&s); return 1; }
    double fps = 25;
    for (uint32_t i = 0; i < s.op.info.nstreams; i++)
        if (s.op.info.s[i].kind == ACM_KIND_VIDEO && s.op.info.s[i].rate_den) { fps = (double)s.op.info.s[i].rate_num / s.op.info.s[i].rate_den; break; }
    static double late[100000], gap[100000];
    struct acm_frame fr;
    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    double c0 = proc_cpu(s.w.pid), t0 = now(), prev = t0;
    int k = 0, drops = 0, copied = 0, hwn = 0;
    if (n <= 0 || n > 100000) n = 100000;
    while (k < n) {
        double due = t0 + k / fps;
        if (realtime) { double w = due - now(); if (w > 0) { struct timespec ts = { (time_t)w, (long)((w - (time_t)w) * 1e9) }; nanosleep(&ts, NULL); } }
        r = next_frame(&s, ACM_WANT_VIDEO | (dmabuf ? ACM_WANT_DMABUF : 0), &fr, NULL, 10000);
        if (r) break;
        double t = now();
        if (k == 0) { t0 = prev = t; due = t; }               /* the schedule starts at the first frame */
        late[k] = (t - due) * 1000;
        gap[k] = (t - prev) * 1000;
        prev = t;
        if (realtime && late[k] > 1000 / fps) drops++;
        copied += !!(fr.flags & ACM_FF_COPIED);
        hwn += !!(fr.flags & ACM_FF_HW);
        k++;
    }
    double wall = now() - t0, cpu = proc_cpu(s.w.pid) - c0;
    getrusage(RUSAGE_SELF, &ru1);
    double bcpu = (double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec + ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec)
                + (double)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec + ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;
    if (r && r != ACM_E_EOF) printf("stopped at %d: %d %s\n", k, r, s.w.why);
    qsort(late, (size_t)k, sizeof late[0], dcmp);
    qsort(gap, (size_t)k, sizeof gap[0], dcmp);
    printf("bench %s: %ux%u path %s%s%s, %d frames in %.2f s = %.1f fps (stream %.2f fps)\n", file, s.op.width, s.op.height,
           pathname(s.op.vpath), copy ? " copy" : "", dmabuf ? " dmabuf" : "", k, wall, k / wall, fps);
    printf("  worker CPU %.2f s = %.0f%% of one core; broker %.2f s; hw frames %d, copied %d\n", cpu, 100 * cpu / wall, bcpu, hwn, copied);
    if (k) {
        printf("  frame interval ms: p50 %.2f p99 %.2f max %.2f\n", gap[k / 2], gap[(k * 99) / 100], gap[k - 1]);
        if (realtime) printf("  lateness vs schedule ms: p50 %.2f p99 %.2f max %.2f; dropped (later than a frame) %d\n",
                             late[k / 2], late[(k * 99) / 100], late[k - 1], drops);
    }
    sess_close(&s);
    return r && r != ACM_E_EOF ? 1 : 0;
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", dir, sizeof dir - 1);
    dir[l > 0 ? l : 0] = 0;
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    snprintf(worker, sizeof worker, "%s", getenv("ACM_WORKER") ? getenv("ACM_WORKER") : "");
    if (!worker[0]) snprintf(worker, sizeof worker, "%s/acmedia-worker", dir);
    snprintf(worker_test, sizeof worker_test, "%s", getenv("ACM_WORKER_TEST") ? getenv("ACM_WORKER_TEST") : "");
    if (!worker_test[0]) snprintf(worker_test, sizeof worker_test, "%s/acmedia-worker-test", dir);
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) { fprintf(stderr, "usage: see the top of acm_test.c\n"); return 2; }
    int hw = 0, copy = 0, dmabuf = 0, realtime = 0, n = 0, quiet = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--hw")) hw = 1;
        else if (!strcmp(argv[i], "--copy")) copy = 1;
        else if (!strcmp(argv[i], "--dmabuf")) dmabuf = 1;
        else if (!strcmp(argv[i], "--realtime")) realtime = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
    }
    const char *c = argv[1], *a = argc > 2 ? argv[2] : "";
    if (!strcmp(c, "info")) return cmd_info(a);
    if (!strcmp(c, "caps")) return cmd_caps();
    if (!strcmp(c, "hash")) return cmd_hash(a, hw, copy, n, quiet);
    if (!strcmp(c, "conformance")) return cmd_conformance(a);
    if (!strcmp(c, "policy")) return cmd_policy(a);
    if (!strcmp(c, "crash")) return cmd_crash(a);
    if (!strcmp(c, "sandbox")) return cmd_sandbox(hw);
    if (!strcmp(c, "fallback")) return cmd_fallback(a);
    if (!strcmp(c, "fuzz")) return cmd_fuzz(a, argc > 3 ? atoi(argv[3]) : 200);
    if (!strcmp(c, "bench")) return cmd_bench(a, hw, copy, dmabuf, realtime, n);
    fprintf(stderr, "acm_test: unknown command %s\n", c);
    return 2;
}
