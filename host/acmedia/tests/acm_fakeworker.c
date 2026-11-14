/*
 * A stand-in acmedia-worker for libacmedia.so's tests (acm_runtime_test.c):
 * the worker protocol (acm_proto.h) with made-up frames, and no FFmpeg. It
 * takes the same arguments and fd 3, and puts itself under no_new_privs and
 * a seccomp filter (one that allows everything) so the broker's lock-down
 * check passes; the real sandbox is the real worker's, tested by stage a.
 *
 * The source is a text line, "FAKE W H FRAMES FPS [TAG] [crash=K] [slots=N]
 * [hang=K] [audio=1] [soft=1] [bad=offset|height|pool]": a video stream of
 * TAG (default H264), W x H, FRAMES frames at FPS, YUV420P in the pool.
 * audio=1 adds 8 kHz stereo S16. Frame k's Y bytes are (k + y + x) & 255, its U k & 255
 * and its V ~k & 255, so the test can check what ACM_FRAME gave. crash=K
 * dies on the K-th frame (SIGSEGV), hang=K stops answering there. With
 * ACM_WANT_DMABUF the frame goes as a memfd standing in for a dma-buf,
 * unless soft=1 forces the pool route
 * (ACM_FF_DMABUF, NV12's fourcc), as the hardware path's do.
 * Anything else is ACM_E_FORMAT at PROBE and OPEN.
 * Team 6, 12 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include "../src/acm_msg.h"
#include "../src/acm_codecs.h"

static int sock = 3, srcfd = -1;
static uint32_t W, H, N, fps, tag = ACM_T_H264, crash_at, hang_at, nslots = 4, has_audio, audio_samples, force_soft;
static uint8_t *pool; static uint32_t slot_size; static int given[64];
static uint32_t k, ak;                               /* the next video/audio data */
static char bad[16];
static struct acm_info info;
static int parsed, opened;

static void err(int code, const char *t) { struct acm_error e = { code, 0, "" }; snprintf(e.text, sizeof e.text, "%s", t); acm_send(sock, ACM_A_ERROR, &e, sizeof e, NULL, 0, NULL, 0); }
static void ok(const void *p, uint32_t n) { acm_send(sock, ACM_A_OK, p, n, NULL, 0, NULL, 0); }

static int parse(void) {
    if (parsed) return parsed > 0;
    char line[256] = { 0 }, t[8] = "H264";
    parsed = -1;
    if (srcfd < 0 || pread(srcfd, line, sizeof line - 1, 0) <= 0) return 0;
    if (sscanf(line, "FAKE %u %u %u %u %7s", &W, &H, &N, &fps, t) < 4 || !W || !H || W > 4096 || H > 4096 || !fps) return 0;
    if (strncmp(t, "crash", 5) && strncmp(t, "slots", 5) && strncmp(t, "hang", 4)) {
        char b[4] = { ' ', ' ', ' ', ' ' }; memcpy(b, t, strlen(t) < 4 ? strlen(t) : 4); tag = ACM_TAG(b[0], b[1], b[2], b[3]);
    }
    char *c = strstr(line, "crash="); if (c) crash_at = (uint32_t)atoi(c + 6);
    c = strstr(line, "hang="); if (c) hang_at = (uint32_t)atoi(c + 5);
    c = strstr(line, "slots="); if (c) { nslots = (uint32_t)atoi(c + 6); if (!nslots || nslots > 64) nslots = 4; }
    has_audio = strstr(line, "audio=1") != NULL;
    c = strstr(line, "audio_samples="); if (c) audio_samples = (uint32_t)atoi(c + 14);
    force_soft = strstr(line, "soft=1") != NULL;
    c = strstr(line, "bad="); if (c) sscanf(c + 4, "%15s", bad);
    memset(&info, 0, sizeof info);
    info.container = ACM_TAG('M', 'K', 'V', ' ');
    info.nstreams = has_audio ? 2 : 1;
    info.duration_ms = (int64_t)N * 1000 / fps;
    snprintf(info.container_name, sizeof info.container_name, "fake");
    info.s[0] = (struct acm_stream){ ACM_KIND_VIDEO, tag, 0, W, H, 8, fps, 1, 0, 0, { 'u', 'n', 'd', ' ' } };
    if (has_audio) info.s[1] = (struct acm_stream){ ACM_KIND_AUDIO, ACM_T_PCM, 0, 0, 0, 16, 0, 0, 8000, 2, { 'u', 'n', 'd', ' ' } };
    parsed = 1;
    return 1;
}

static void fill(uint8_t *b, uint32_t f) {
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) b[y * W + x] = (uint8_t)(f + y + x);
    uint32_t cw = (W + 1) / 2, ch = (H + 1) / 2;
    memset(b + W * H, (int)(f & 255), cw * ch);
    memset(b + W * H + cw * ch, (int)(~f & 255), cw * ch);
}

static void next(uint32_t want) {
    if (!opened) { err(ACM_E_BADARG, "nothing open"); return; }
    if (want & ACM_WANT_AUDIO) {
        uint32_t total = audio_samples ? audio_samples : N * 8000 / fps;
        if (!has_audio || ak >= total) { err(ACM_E_EOF, "audio end"); return; }
        uint32_t frames = total - ak < 128 ? total - ak : 128;
        uint8_t b[sizeof(struct acm_audio) + 512];
        struct acm_audio a = { (int64_t)ak * 1000 / 8000, frames, 8000, 2, 0 };
        memcpy(b, &a, sizeof a);
        int16_t *pcm = (int16_t *)(void *)(b + sizeof a);
        for (uint32_t i = 0; i < frames; i++) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)((ak + i) & 0x7fff);
        ak += frames;
        acm_send(sock, ACM_A_AUDIO, b, (uint32_t)(sizeof a + frames * 4), NULL, 0, NULL, 0);
        return;
    }
    if (k >= N) { err(ACM_E_EOF, "end"); return; }
    if (crash_at && k + 1 == crash_at) raise(SIGSEGV);
    if (hang_at && k + 1 == hang_at) for (;;) pause();
    uint32_t cw = (W + 1) / 2, ch = (H + 1) / 2;
    struct acm_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.pts_ms = (int64_t)k * 1000 / fps; fr.width = W; fr.height = H; fr.path = ACM_PATH_SW;
    if ((want & ACM_WANT_DMABUF) && !force_soft) {
        static uint32_t hwk;
        int fd = memfd_create("fake-dmabuf", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)(W * H * 3 / 2 + 16))) { err(ACM_E_LIMIT, "memfd"); return; }
        fr.flags = ACM_FF_DMABUF | ACM_FF_HW; fr.slot = 0x80000000u | (hwk++ & 0xff); fr.pixfmt = ACM_PIX_NV12;
        fr.nplanes = 2; fr.nobj = 1; fr.drm_fourcc = 0x3231564e;
        fr.pitch[0] = W; fr.pitch[1] = W; fr.offset[1] = W * H;
        k++;
        acm_send(sock, ACM_A_FRAME, &fr, sizeof fr, NULL, 0, &fd, 1);
        close(fd);
        return;
    }
    int s = -1;
    for (uint32_t i = 0; i < nslots; i++) if (!given[i]) { s = (int)i; break; }
    if (s < 0) { err(ACM_E_AGAIN, "every slot is out"); return; }
    fill(pool + (size_t)s * slot_size, k);
    given[s] = 1;
    fr.slot = (uint32_t)s; fr.pixfmt = ACM_PIX_YUV420P; fr.nplanes = 3;
    fr.offset[0] = 0; fr.pitch[0] = W; fr.rowbytes[0] = W; fr.rows[0] = H;
    fr.offset[1] = W * H; fr.pitch[1] = cw; fr.rowbytes[1] = cw; fr.rows[1] = ch;
    fr.offset[2] = W * H + cw * ch; fr.pitch[2] = cw; fr.rowbytes[2] = cw; fr.rows[2] = ch;
    if (!strcmp(bad, "offset")) fr.offset[1] = slot_size;
    if (!strcmp(bad, "height")) fr.height = H + 1;
    k++;
    acm_send(sock, ACM_A_FRAME, &fr, sizeof fr, NULL, 0, NULL, 0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--fd") && i + 1 < argc) sock = atoi(argv[++i]);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter allow[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    struct sock_fprog prog = { 1, allow };
    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
    static uint8_t rx[ACM_MSG_MAX];
    for (;;) {
        uint32_t t, len; int fds[ACM_MAX_FDS], nf = ACM_MAX_FDS;
        int r = acm_recv(sock, &t, rx, sizeof rx, &len, fds, &nf);
        if (r <= 0) return 0;
        switch (t) {
        case ACM_Q_HELLO: { struct acm_ready rd = { ACM_PROTO_VERSION, ACM_SB_SECCOMP | ACM_SB_NNP, 0, 0 }; ok(&rd, sizeof rd); break; }
        case ACM_Q_SOURCE: if (nf == 1) { srcfd = fds[0]; ok(NULL, 0); } else err(ACM_E_BADARG, "no fd"); break;
        case ACM_Q_PROBE: if (parse()) acm_send(sock, ACM_A_INFO, &info, sizeof info, NULL, 0, NULL, 0); else err(ACM_E_FORMAT, "not a fake"); break;
        case ACM_Q_OPEN: {
            if (!parse()) { err(ACM_E_FORMAT, "not a fake"); break; }
            const struct acm_open *o = (const void *)rx;
            struct acm_opened op;
            memset(&op, 0, sizeof op);
            op.video = o->video == -2 ? -1 : 0;
            op.audio = o->audio == -2 || !has_audio ? -1 : 1;
            op.vpath = op.video >= 0 ? ACM_PATH_SW : ACM_PATH_NONE;
            op.apath = op.audio >= 0 ? ACM_PATH_SW : ACM_PATH_NONE;
            op.width = W; op.height = H; op.pixfmt = ACM_PIX_YUV420P;
            op.slot_size = (W * H * 3 / 2 + 4095) & ~4095u; op.nslots = op.video < 0 ? 0 : nslots;
            if (op.audio >= 0) { op.sample_rate = 8000; op.channels = 2; }
            if (!strcmp(bad, "pool")) { op.slot_size = 0x40000000u; op.nslots = 2; }
            op.info = info;
            opened = op.video >= 0 || op.audio >= 0;
            acm_send(sock, ACM_A_OPENED, &op, sizeof op, NULL, 0, NULL, 0);
            break;
        }
        case ACM_Q_POOL: {
            const struct acm_pool *p = (const void *)rx;
            if (nf != 1) { err(ACM_E_BADARG, "no pool"); break; }
            pool = mmap(NULL, p->size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
            close(fds[0]);
            if (pool == MAP_FAILED) { pool = NULL; err(ACM_E_LIMIT, "mmap"); break; }
            slot_size = p->slot_size;
            ok(NULL, 0);
            break;
        }
        case ACM_Q_NEXT: next(((const struct acm_next *)rx)->want); break;
        case ACM_Q_RELEASE: {
            uint32_t s = ((const struct acm_release *)rx)->slot;
            if (s & 0x80000000u) { ok(NULL, 0); break; }
            if (s >= nslots || !given[s]) { err(ACM_E_BADARG, "slot not given out"); break; }
            given[s] = 0; ok(NULL, 0);
            break;
        }
        case ACM_Q_SEEK: {
            int64_t ms = ((const struct acm_seek *)rx)->ms;
            uint32_t f = (uint32_t)(ms * fps / 1000);
            if (f >= N) { err(ACM_E_EOF, "past the end"); break; }
            k = f; ak = (uint32_t)(ms * 8); ok(NULL, 0);
            break;
        }
        case ACM_Q_CAPS: {
            struct acm_caps c; memset(&c, 0, sizeof c);
            c.n = 1; c.c[0] = (struct acm_hwcap){ ACM_T_HEVC, ACM_PATH_HW_V4L2REQ, 4096, 2304, (1u << 8) | (1u << 10), "fake" };
            acm_send(sock, ACM_A_CAPS, &c, sizeof c, NULL, 0, NULL, 0);
            break;
        }
        case ACM_Q_CLOSE: ok(NULL, 0); return 0;
        default: err(ACM_E_BADARG, "unknown"); break;
        }
    }
}
