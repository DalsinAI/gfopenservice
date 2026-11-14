/*
 * libacmedia.so as the runtime loads it (dlopen, acmedia_backend_v1 with a
 * stand-in acm_host), against acm_fakeworker (no FFmpeg) and a stand-in
 * acscreen on a SEQPACKET socket. The backend's side of M4K step 4: CAPS with
 * the policy and the host's hardware, PROBE and RESULT, OPEN to GUEST and to
 * PLANE (and PLANE with no acscreen, and acscreen turning the plane off),
 * PLAY/PAUSE/STOP/SEEK/RATE, the frames ACM_FRAME gives checked byte for
 * byte, END, a worker that crashes (ERROR, DECODER, FAULT's line, the next
 * session plays), CLOSE while playing, and no worker left behind.
 *
 *   acm_runtime_test LIBACMEDIA FAKEWORKER [--real FILE]
 *   acm_runtime_test LIBACMEDIA WORKER --real-flow[-paced|-unread] FILE FRAMES SAMPLES
 * Team 6, 12 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../src/acm_guest.h"
#include "../src/acm_proto.h"
#include "../src/acm_codecs.h"
#include "../src/acvideo_proto.h"
#include "../src/acmedia_service.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint32_t rd(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint64_t ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }

/* ---- the runtime, stood in for: events and sources ---- */
static pthread_mutex_t emu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ecv = PTHREAD_COND_INITIALIZER;
struct ev { uint32_t h, type, a, b; };
static struct ev evs[8192];
static int nev;
static void host_post(uint32_t h, uint32_t type, uint32_t a, uint32_t b) {
    pthread_mutex_lock(&emu);
    if (nev < 8192) evs[nev++] = (struct ev){ h, type, a, b };
    pthread_cond_broadcast(&ecv);
    pthread_mutex_unlock(&emu);
}
/* the first event of type for h at or after *from, within ms: its index, or -1 */
static int wait_ev(uint32_t h, uint32_t type, int *from, int ms, struct ev *out) {
    uint64_t end = ms_now() + (uint64_t)ms;
    pthread_mutex_lock(&emu);
    for (;;) {
        for (int i = *from; i < nev; i++)
            if (evs[i].h == h && evs[i].type == type) { if (out) *out = evs[i]; *from = i + 1; pthread_mutex_unlock(&emu); return i; }
        if (ms_now() >= end) { pthread_mutex_unlock(&emu); return -1; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 20000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&ecv, &emu, &ts);
    }
}
static int count_ev(uint32_t h, uint32_t type, int from) {
    int n = 0;
    pthread_mutex_lock(&emu);
    for (int i = from; i < nev; i++) n += evs[i].h == h && evs[i].type == type;
    pthread_mutex_unlock(&emu);
    return n;
}
static int nev_now(void) { pthread_mutex_lock(&emu); int n = nev; pthread_mutex_unlock(&emu); return n; }

static char dir[64], real_dir[4096];
static int real_mode;
static int sources;
/* ACM_SRC_ACFS, unit 0: a file in dir */
static int32_t host_source(const uint8_t *src, uint32_t n, int *fd) {
    *fd = -1;
    if (n < 9 || rd(src) != 1) return ACM_E_BADARG;
    char p[4096];
    const char *base = real_mode ? real_dir : dir;
    size_t bl = strlen(base), nl = strnlen((const char *)src + 8, n - 8);
    if (nl == n - 8 || bl + 1 + nl + 1 > sizeof p) return ACM_E_BADARG;
    memcpy(p, base, bl); p[bl] = '/'; memcpy(p + bl + 1, src + 8, nl + 1);
    *fd = open(p, O_RDONLY | O_CLOEXEC);
    sources++;
    return *fd < 0 ? ACM_E_IO : 0;
}
static const acm_host host = { ACM_SVC_ABI, host_post, host_source };

static uint32_t srcd(uint8_t *d, const char *name) {
    be(d, 1); be(d + 4, 0);
    size_t l = strlen(name) + 1;
    memcpy(d + 8, name, l);
    return (uint32_t)(8 + l);
}
static void file(const char *name, const char *text) {
    char p[256]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}

/* ---- a stand-in acscreen: each frame shown at once, the one before DONE; STATE off on demand ---- */
static char vsock[100];
static int vl = -1;
static _Atomic int vframes, vcopies, vhide, vfds_ok = 1, vrects, voff_req, voff_sent;
static void vsend(int c, uint32_t type, const void *b, uint32_t len) {
    uint8_t p[ACV_MAX_PACKET]; struct acv_hdr h = { ACV_MAGIC, type, len, 0 };
    memcpy(p, &h, sizeof h); if (len) memcpy(p + sizeof h, b, len);
    send(c, p, sizeof h + len, MSG_NOSIGNAL);
}
static void *acscreen(void *a) {
    (void)a;
    for (;;) {
        int c = accept(vl, NULL, NULL);
        if (c < 0) return NULL;
        uint64_t prev = 0;
        for (;;) {
            uint8_t b[ACV_MAX_PACKET];
            union { char c[CMSG_SPACE(sizeof(int) * ACV_MAX_OBJ)]; struct cmsghdr h; } cm;
            struct iovec iov = { b, sizeof b };
            struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm.c, .msg_controllen = sizeof cm.c };
            struct timeval tv = { 0, 20000 };
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ssize_t n = recvmsg(c, &m, 0);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                if (voff_req && !voff_sent) { struct acv_state st = { 0, ACV_W_NOT_SHOWN, 1920, 1080 }; vsend(c, ACV_STATE, &st, sizeof st); voff_sent = 1; }
                continue;
            }
            if (n <= 0) break;
            struct acv_hdr h; memcpy(&h, b, sizeof h);
            int nf = 0;
            for (struct cmsghdr *x = CMSG_FIRSTHDR(&m); x; x = CMSG_NXTHDR(&m, x)) {
                int k = (int)((x->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < k; i++) { int fd; memcpy(&fd, CMSG_DATA(x) + sizeof(int) * (size_t)i, sizeof fd); close(fd); nf++; }
            }
            if (h.type == ACV_HELLO) { struct acv_welcome w = { ACV_VERSION, ACV_WF_COPY, 1920, 1080, 60000, 640, 480, 1, 0, 0 }; vsend(c, ACV_WELCOME, &w, sizeof w); }
            else if (h.type == ACV_RECT) vrects++;
            else if (h.type == ACV_HIDE) { vhide++; if (prev) { struct acv_done d = { prev, ACV_W_HIDDEN, 0 }; vsend(c, ACV_DONE, &d, sizeof d); prev = 0; } }
            else if (h.type == ACV_FRAME || h.type == ACV_COPY) {
                uint64_t id; memcpy(&id, b + sizeof h, sizeof id);
                if (h.type == ACV_FRAME) {
                    struct acv_frame f; memcpy(&f, b + sizeof h, sizeof f);
                    if (nf != (int)f.nobj || nf < 1) vfds_ok = 0;
                } else { if (nf != 1) vfds_ok = 0; vcopies++; }
                vframes++;
                if (voff_sent) { struct acv_done d = { id, ACV_W_NOT_SHOWN, 0 }; vsend(c, ACV_DONE, &d, sizeof d); continue; }
                struct acv_shown s = { id, 0, (uint32_t)vframes, 0 }; vsend(c, ACV_SHOWN, &s, sizeof s);
                if (prev) { struct acv_done d = { prev, ACV_W_OK, 0 }; vsend(c, ACV_DONE, &d, sizeof d); }
                prev = id;
                if (voff_req && !voff_sent) {
                    struct acv_state st = { 0, ACV_W_NOT_SHOWN, 1920, 1080 }; vsend(c, ACV_STATE, &st, sizeof st); voff_sent = 1;
                    struct acv_done d = { prev, ACV_W_HIDDEN, 0 }; vsend(c, ACV_DONE, &d, sizeof d); prev = 0;
                }
            }
        }
        close(c);
    }
}

static int workers(void) {                    /* acm_fakeworker processes that are our children */
    int n = 0; DIR *d = opendir("/proc"); struct dirent *e;
    char me[32]; snprintf(me, sizeof me, "%d", (int)getpid());
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300], b[512]; snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); b[k] = 0;
        char *r = strrchr(b, ')');
        if (!strstr(b, "(acm_fakeworker") || !r) continue;
        char st; int ppid; if (sscanf(r + 2, "%c %d", &st, &ppid) == 2 && ppid == getpid()) n++;
    }
    if (d) closedir(d);
    return n;
}

static const acm_backend *B;
static uint8_t vtarget[32], gtarget[32];

/* a session: open, OPENED; its handle (slot | 0x100 * gen) */
static uint32_t opened(uint32_t slot, uint32_t gen, const char *name, const uint8_t *vt, uint32_t flags, int32_t *r) {
    uint8_t d[256]; uint32_t n = srcd(d, name);
    uint32_t h = gen << 8 | slot;
    *r = B->open(B->ctx, slot, h, d, n, ACMG_ST_VIDEO, vt, NULL, 0, flags);
    return h;
}

/* frame k of a W x H fake, as ACM_FRAME should give it (pitch p) */
static int frame_ok(const uint8_t *b, uint32_t W, uint32_t H, uint32_t p, uint32_t k) {
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) if (b[y * p + x] != (uint8_t)(k + y + x)) return 0;
    const uint8_t *u = b + (size_t)p * H, *v = u + (size_t)(p / 2) * ((H + 1) / 2);
    for (uint32_t y = 0; y < (H + 1) / 2; y++) for (uint32_t x = 0; x < (W + 1) / 2; x++)
        if (u[y * (p / 2) + x] != (uint8_t)k || v[y * (p / 2) + x] != (uint8_t)~k) return 0;
    return 1;
}

/* The real-file receipt: the dynamically linked worker decodes the checked-in
 * H.264/AAC clip through libacmedia, not through the direct worker harness.
 * The sink proves one S16 block with a positive byte count and one video
 * frame. Timing is obtained separately through INFO and events. */
static int real_file(const char *libpath, const char *worker, const char *clip) {
    char name[512];
    real_mode = 1;
    snprintf(real_dir, sizeof real_dir, "%s", clip);
    char *sl = strrchr(real_dir, '/');
    if (!sl) { snprintf(name, sizeof name, "%s", clip); snprintf(real_dir, sizeof real_dir, "."); }
    else { snprintf(name, sizeof name, "%s", sl + 1); *sl = 0; }
    setenv("AC_ACMEDIA_WORKER", worker, 1);
    setenv("AC_ACMEDIA_HW", "0", 1);
    unsetenv("AC_ACMEDIA_DENY");
    unsetenv("AC_ACMEDIA_HWONLY");
    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    acm_backend_entry entry = lib ? (acm_backend_entry)dlsym(lib, "acmedia_backend_v1") : NULL;
    if (!entry || !(B = entry(&host))) { fprintf(stderr, "real: libacmedia unavailable: %s\n", dlerror()); return 1; }
    uint8_t d[1024], vt[32] = { 0 }, at[32] = { 0 }, info[4096];
    be(vt, ACMG_OUT_GUEST); be(at, ACMG_OUT_GUEST);
    uint32_t h = 0x101, n = srcd(d, name);
    int from = nev_now();
    struct ev e;
    int32_t r = B->open(B->ctx, 1, h, d, n, ACMG_ST_VIDEO | ACMG_ST_AUDIO, vt, at, 0, ACMG_OF_EVENTS_FRAME);
    if (r != ACM_PENDING || wait_ev(h, ACMG_EV_OPENED, &from, 15000, &e) < 0) { fprintf(stderr, "real: OPEN failed (%d)\n", r); return 1; }
    r = B->info(B->ctx, 1, info, sizeof info);
    uint32_t ai = rd(info + 4 * ACM_IH_AUDIO);
    if (r < 0 || rd(info + 4 * ACM_IH_VIDEO) == 0xffffffffu || ai == 0xffffffffu ||
        rd(info + 4 * (ACM_INFO_HEAD_LONGS + ai * ACM_INFO_STREAM_LONGS + ACM_IS_CHANNELS)) != 2) {
        fprintf(stderr, "real: INFO has no A/V streams (%d)\n", r); return 1;
    }
    uint32_t a4[4] = { 0, 0, 0, 0 };
    if (B->control(B->ctx, 1, ACMG_CTL_PLAY, a4)) { fprintf(stderr, "real: PLAY failed\n"); return 1; }
    uint8_t pcm[ACM_MSG_MAX];
    memset(pcm, 0, sizeof pcm);
    int32_t abytes = ACM_E_AGAIN;
    for (int tries = 0; tries < 1000 && abytes == ACM_E_AGAIN; tries++) {
        abytes = B->audio(B->ctx, 1, pcm, sizeof pcm, ACMG_SND_S16, 48000);
        if (abytes == ACM_E_AGAIN) usleep(2000);
    }
    int nonzero = 0;
    if (abytes > 0) for (int32_t i = 0; i < abytes; i++) nonzero |= pcm[i];
    if (abytes <= 0 || (abytes & 1) || !nonzero) { fprintf(stderr, "real: no S16 audio (%d bytes)\n", abytes); return 1; }
    if (wait_ev(h, ACMG_EV_FRAME, &from, 5000, &e) < 0) { fprintf(stderr, "real: no video event\n"); return 1; }
    uint8_t *frame = malloc(640u * 360u * 2u);
    int32_t vpts = frame ? B->frame(B->ctx, 1, frame, 640u * 360u * 2u, 640, ACMG_PIX_YUV420, 640, 360) : ACM_E_LIMIT;
    if (vpts < 0) { fprintf(stderr, "real: no video frame (%d)\n", vpts); free(frame); return 1; }
    uint32_t sum = 0;
    for (size_t i = 0; i < 640u * 360u * 3u / 2u; i++) sum = sum * 33u + frame[i];
    free(frame);
    B->close(B->ctx, 1);
    printf("acm_runtime_real: PASS video_pts=%d video_checksum=%08x audio_bytes=%d audio=48000Hz/S16BE/stereo\n", vpts, sum, abytes);
    return 0;
}

/* The review stress case through the public backend: consume audio as fast
 * as offered while video stays on its real-time clock. Read-ahead must be
 * back-pressured, not overflow the worker's crossed-video queue. */
static int real_flow(const char *libpath, const char *worker, const char *clip, int want_frames, int want_samples,
                     int pace, int unread) {
    char name[512];
    real_mode = 1;
    snprintf(real_dir, sizeof real_dir, "%s", clip);
    char *sl = strrchr(real_dir, '/');
    if (!sl) { snprintf(name, sizeof name, "%s", clip); snprintf(real_dir, sizeof real_dir, "."); }
    else { snprintf(name, sizeof name, "%s", sl + 1); *sl = 0; }
    setenv("AC_ACMEDIA_WORKER", worker, 1);
    setenv("AC_ACMEDIA_HW", "0", 1);
    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    acm_backend_entry entry = lib ? (acm_backend_entry)dlsym(lib, "acmedia_backend_v1") : NULL;
    if (!entry || !(B = entry(&host))) { fprintf(stderr, "real-flow: libacmedia unavailable: %s\n", dlerror()); return 1; }
    uint8_t d[1024], vt[32] = { 0 }, at[32] = { 0 }, pcm[ACM_MSG_MAX];
    be(vt, ACMG_OUT_GUEST); be(at, ACMG_OUT_GUEST);
    uint32_t h = 0x101, n = srcd(d, name), a4[4] = { 0 };
    int from = nev_now(), event_start = from;
    struct ev e;
    int32_t r = B->open(B->ctx, 1, h, d, n, ACMG_ST_VIDEO | ACMG_ST_AUDIO, vt, at, 0, ACMG_OF_EVENTS_FRAME);
    if (r != ACM_PENDING || wait_ev(h, ACMG_EV_OPENED, &from, 15000, &e) < 0 || B->control(B->ctx, 1, ACMG_CTL_PLAY, a4)) {
        fprintf(stderr, "real-flow: OPEN/PLAY failed (%d)\n", r); return 1;
    }
    int samples = 0, ended = 0, failed = 0;
    uint64_t deadline = ms_now() + (uint64_t)(want_frames > 0 ? want_frames * 1000 / 20 + 30000 : 60000);
    while (ms_now() < deadline && !ended && !failed) {
        r = unread ? ACM_E_AGAIN : B->audio(B->ctx, 1, pcm, sizeof pcm, ACMG_SND_S16, 0);
        if (r > 0) {
            int got = r / 4;
            samples += got;
            if (pace) usleep((useconds_t)((uint64_t)got * 1000000 / 48000));
        } else if (r != ACM_E_AGAIN && r != ACM_E_EOF) failed = r;
        pthread_mutex_lock(&emu);
        for (int i = event_start; i < nev; i++) if (evs[i].h == h) {
            if (evs[i].type == ACMG_EV_END) ended = 1;
            if (evs[i].type == ACMG_EV_ERROR) failed = (int32_t)evs[i].a;
        }
        pthread_mutex_unlock(&emu);
        if (r == ACM_E_AGAIN) usleep(500);
    }
    /* END may be posted with the final PCM blocks still waiting for the guest. */
    if (!unread) {
        uint64_t drain_deadline = ms_now() + 5000;
        for (;;) {
            r = B->audio(B->ctx, 1, pcm, sizeof pcm, ACMG_SND_S16, 0);
            if (r > 0) { samples += r / 4; continue; }
            if (r == ACM_E_AGAIN && ms_now() < drain_deadline) { usleep(500); continue; }
            break;
        }
    }
    int frames = count_ev(h, ACMG_EV_FRAME, event_start);
    B->close(B->ctx, 1);
    printf("acm_runtime_real_flow%s%s: %d/%d frames, %d/%d samples, end=%d error=%d\n",
           pace ? " paced" : "", unread ? " unread" : "", frames, want_frames, samples, want_samples, ended, failed);
    return !ended || failed || frames != want_frames || samples != want_samples;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: acm_runtime_test LIBACMEDIA WORKER [--real FILE | --real-flow[-paced|-unread] FILE FRAMES SAMPLES]\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    if (argc == 5 && !strcmp(argv[3], "--real")) return real_file(argv[1], argv[2], argv[4]);
    if (argc == 7 && !strcmp(argv[3], "--real-flow"))
        return real_flow(argv[1], argv[2], argv[4], atoi(argv[5]), atoi(argv[6]), 0, 0);
    if (argc == 7 && !strcmp(argv[3], "--real-flow-paced"))
        return real_flow(argv[1], argv[2], argv[4], atoi(argv[5]), atoi(argv[6]), 1, 0);
    if (argc == 7 && !strcmp(argv[3], "--real-flow-unread"))
        return real_flow(argv[1], argv[2], argv[4], atoi(argv[5]), atoi(argv[6]), 0, 1);
    snprintf(dir, sizeof dir, "/tmp/acmrtXXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(vsock, sizeof vsock, "%s/video.sock", dir);

    /* no worker: no backend (in a child, as the library sets up once a process) */
    pid_t pid = fork();
    if (!pid) {
        setenv("AC_ACMEDIA_WORKER", "/nonexistent/acmedia-worker", 1);
        void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        acm_backend_entry e = h ? (acm_backend_entry)dlsym(h, "acmedia_backend_v1") : NULL;
        _exit(!e ? 3 : e(&host) ? 1 : 0);
    }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "no worker: no backend (%d)", st);

    setenv("AC_ACMEDIA_WORKER", argv[2], 1);
    setenv("AC_VIDEO_SOCK", vsock, 1);
    setenv("AC_ACMEDIA_DENY", "AV1", 1);
    setenv("AC_ACMEDIA_HWONLY", "VP9", 1);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    acm_backend_entry entry = lib ? (acm_backend_entry)dlsym(lib, "acmedia_backend_v1") : NULL;
    CHECK(entry != NULL, "libacmedia.so exports acmedia_backend_v1 (%s)", dlerror());
    if (!entry) return 1;
    acm_host bad = host; bad.abi = 99;
    CHECK(entry(&bad) == NULL, "a host of another ABI: none");
    B = entry(&host);
    CHECK(B && B->abi == ACM_SVC_ABI && B->caps && B->probe && B->result && B->open && B->info && B->control && B->frame && B->close && B->fault,
          "the backend, whole");
    if (!B) return 1;

    /* CAPS */
    static uint8_t caps[4096];
    CHECK(B->caps(B->ctx, 2, caps, sizeof caps) == ACM_E_VERSION, "CAPS of version 2: ACM_E_VERSION");
    CHECK(B->caps(B->ctx, 1, caps, 40) == ACM_E_BADARG, "CAPS into less than its head: ACM_E_BADARG");
    uint64_t t0 = ms_now();
    int32_t n;
    for (;;) { n = B->caps(B->ctx, 1, caps, sizeof caps); if (!(rd(caps + 4 * ACM_CH_FLAGS) & ACM_CHF_HWSCAN) || ms_now() - t0 > 5000) break; usleep(10000); }
    CHECK(n > 30 && rd(caps + 4 * ACM_CH_ENTRIES) == (uint32_t)n && rd(caps) == 1 && rd(caps + 4 * ACM_CH_SESSIONS) == ACM_SVC_SESSIONS
          && rd(caps + 4 * ACM_CH_RANGE_MAX) == 16u << 20 && rd(caps + 4 * ACM_CH_CALLS) == 1 && rd(caps + 4 * (ACM_CH_CALLS + 11)) == 1,
          "CAPS: the head (%d entries)", n);
    int hevc = 0, av1 = 0, vp9 = 0, h264 = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = caps + 4 * (ACM_CAPS_HEAD_LONGS + i * ACM_CAPS_ENTRY_LONGS);
        uint32_t tag = rd(e), fl = rd(e + 4);
        if (tag == ACM_T_HEVC) hevc = fl == (ACM_CEF_BUILT | ACM_CEF_ALLOWED | ACM_CEF_HW) && rd(e + 8) == ACMG_PATH_HW_V4L2_REQUEST && rd(e + 12) == 4096 && rd(e + 20) == ((1u << 8) | (1u << 10));
        if (tag == ACM_T_AV1) av1 = fl == ACM_CEF_BUILT;
        if (tag == ACM_T_VP9) vp9 = fl == (ACM_CEF_BUILT | ACM_CEF_HWONLY);
        if (tag == ACM_T_H264) h264 = fl == (ACM_CEF_BUILT | ACM_CEF_ALLOWED);
    }
    CHECK(hevc, "CAPS: HEVC on the host's hardware decoder (V4L2 request, 4096 wide, 8 and 10 bit)");
    CHECK(av1 && vp9 && h264, "CAPS: the policy (AV1 denied, VP9 hardware only, H264 allowed)");
    CHECK(B->caps(B->ctx, 1, caps, 4 * (ACM_CAPS_HEAD_LONGS + 2 * ACM_CAPS_ENTRY_LONGS) + 4) == 2, "CAPS: as many entries as len holds");

    /* PROBE and RESULT */
    file("film.fake", "FAKE 64 48 10 25\n");
    file("short.fake", "FAKE 34 18 6 50 HEVC\n");
    file("junk.bin", "not a film\n");
    file("crash.fake", "FAKE 32 16 20 25 crash=3\n");
    file("long.fake", "FAKE 64 48 100 25 soft=1\n");
    file("av.fake", "FAKE 64 48 10 25 audio=1\n");
    file("av-unread.fake", "FAKE 64 48 25 25 audio=1\n");
    file("av-tail.fake", "FAKE 64 48 4 25 audio=1 audio_samples=6400\n");
    uint8_t d[256], out[4096];
    int from = 0;
    struct ev e;
    int32_t tk = B->probe(B->ctx, d, srcd(d, "film.fake"), out, sizeof out);
    CHECK(tk > 0, "PROBE: a ticket (%d)", tk);
    int32_t r0 = B->result(B->ctx, (uint32_t)tk, out, sizeof out);
    CHECK(wait_ev(0, ACMG_EV_PROBED, &from, 5000, &e) >= 0 && e.a == (uint32_t)tk, "PROBED, with the ticket");
    int32_t r = B->result(B->ctx, (uint32_t)tk, out, sizeof out);
    CHECK((r0 == ACM_PENDING || r0 == r) && r == 4 * (ACM_INFO_HEAD_LONGS + ACM_INFO_STREAM_LONGS), "RESULT: pending, then the block (%d, %d)", r0, r);
    CHECK(rd(out) == ACM_TAG('M', 'K', 'V', ' ') && rd(out + 4) == 1 && rd(out + 8) == 400 && rd(out + 4 * ACM_IH_VIDEO) == 0xffffffffu, "RESULT: container, streams, duration");
    const uint8_t *s0 = out + 4 * ACM_INFO_HEAD_LONGS;
    CHECK(rd(s0) == ACM_KIND_VIDEO && rd(s0 + 4) == ACM_T_H264 && rd(s0 + 12) == 64 && rd(s0 + 16) == 48 && rd(s0 + 24) == 25 && rd(s0 + 40) == ACM_TAG('u', 'n', 'd', ' '),
          "RESULT: the stream");
    CHECK(B->result(B->ctx, (uint32_t)tk, out, sizeof out) == ACM_E_NOSUCH, "RESULT read once: then ACM_E_NOSUCH");
    tk = B->probe(B->ctx, d, srcd(d, "junk.bin"), out, sizeof out);
    CHECK(tk > 0 && wait_ev(0, ACMG_EV_PROBED, &from, 5000, &e) >= 0 && B->result(B->ctx, (uint32_t)tk, out, sizeof out) == ACM_E_FORMAT,
          "PROBE of something else: RESULT ACM_E_FORMAT");
    CHECK(B->probe(B->ctx, d, srcd(d, "missing.fake"), out, sizeof out) == ACM_E_IO, "PROBE of a missing file: ACM_E_IO at once");
    CHECK(B->result(B->ctx, 0x12345, out, sizeof out) == ACM_E_NOSUCH && B->result(B->ctx, 0, out, sizeof out) == ACM_E_NOSUCH, "RESULT of no ticket: ACM_E_NOSUCH");

    /* OPEN to GUEST, the frames */
    be(gtarget, ACMG_OUT_GUEST);
    from = nev_now();
    uint32_t h = opened(1, 1, "film.fake", gtarget, ACMG_OF_EVENTS_FRAME, &r);
    CHECK(r == ACM_PENDING, "OPEN: ACM_PENDING (%d)", r);
    CHECK(wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e) >= 0 && e.a == 400 && e.b == ACMG_PATH_SW, "OPENED: the duration and the path");
    r = B->info(B->ctx, 1, out, sizeof out);
    CHECK(r == 96 && rd(out + 4 * ACM_IH_STATE) == ACM_STATE_PAUSED && rd(out + 4 * ACM_IH_VIDEO) == 0 && rd(out + 4 * ACM_IH_VROUTE) == ACMG_ROUTE_GUEST
          && rd(out + 4 * ACM_IH_AROUTE) == ACMG_ROUTE_NONE && rd(out + 4 * ACM_IH_SIZE) == (64u << 16 | 48) && rd(out + 4 * ACM_IH_VPATH) == ACMG_PATH_SW,
          "INFO: paused, video 0, route GUEST, no audio, 64x48, software (%d)", r);
    CHECK(B->info(B->ctx, 1, out, 20) == ACM_E_BADARG, "INFO into less than its head: ACM_E_BADARG");
    static uint8_t fb[64 * 48 * 2];
    CHECK(B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 64, 48) == ACM_E_AGAIN, "FRAME before PLAY: ACM_E_AGAIN");
    uint32_t a4[4] = { 0, 0, 0, 0 };
    CHECK(B->control(B->ctx, 1, ACMG_CTL_PLAY, a4) == 0, "PLAY");
    CHECK(wait_ev(h, ACMG_EV_FRAME, &from, 3000, &e) >= 0, "FRAME events (ACM_OF_EVENTS_FRAME)");
    int32_t pts = B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 64, 48);
    CHECK(pts >= 0 && pts % 40 == 0 && frame_ok(fb, 64, 48, 64, (uint32_t)pts / 40), "FRAME: the frame of its pts, byte for byte (pts %d)", pts);
        CHECK(B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_ARGB32, 64, 48) == ACM_E_FORMAT, "FRAME as ARGB32: ACM_E_FORMAT (stage c)");
    CHECK(B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 32, 24) == ACM_E_FORMAT, "FRAME at another size: ACM_E_FORMAT (no scaling)");
    CHECK(B->control(B->ctx, 1, ACMG_CTL_PAUSE, a4) == 0, "PAUSE");
    usleep(150000);
    r = B->info(B->ctx, 1, out, sizeof out);
    uint32_t pos = rd(out + 4 * ACM_IH_POSITION);
    usleep(200000);
    B->info(B->ctx, 1, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_STATE) == ACM_STATE_PAUSED && rd(out + 4 * ACM_IH_POSITION) == pos, "paused: the position stays (%u)", pos);
    /* the newest due frame, once */
    int32_t p1 = B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 0, 0);
    int32_t p2 = B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 0, 0);
    CHECK((p1 >= 0 || p1 == ACM_E_AGAIN) && p2 == ACM_E_AGAIN, "a frame is given once (%d, %d)", p1, p2);
    CHECK(B->frame(B->ctx, 1, fb, 100, 64, ACMG_PIX_YUV420, 0, 0) == ACM_E_AGAIN, "nothing new: ACM_E_AGAIN before the size is looked at");
    a4[0] = 200; a4[1] = ACMG_SEEK_EXACT;
    CHECK(B->control(B->ctx, 1, ACMG_CTL_SEEK, a4) == 0, "SEEK 200 ms");
    usleep(100000);
    B->info(B->ctx, 1, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_POSITION) == 200, "the position after SEEK (%u)", rd(out + 4 * ACM_IH_POSITION));
    a4[0] = 0;
    CHECK(B->control(B->ctx, 1, ACMG_CTL_PLAY, a4) == 0, "PLAY again");
    CHECK(wait_ev(h, ACMG_EV_FRAME, &from, 3000, &e) >= 0 && e.a >= 200, "frames from the seek on (%u)", e.a);
    pts = B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 64, 48);
    CHECK(pts == ACM_E_AGAIN || (pts >= 200 && frame_ok(fb, 64, 48, 64, (uint32_t)pts / 40)), "FRAME after the seek (%d)", pts);
    CHECK(wait_ev(h, ACMG_EV_END, &from, 3000, &e) >= 0 && e.a == 360, "END, at the last frame's time (%u)", e.a);
    B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 64, 48);
    CHECK(B->frame(B->ctx, 1, fb, sizeof fb, 64, ACMG_PIX_YUV420, 64, 48) == ACM_E_EOF, "FRAME at the end: ACM_E_EOF");
    B->info(B->ctx, 1, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_STATE) == ACM_STATE_ENDED, "INFO: ended");
    CHECK(B->control(B->ctx, 1, ACMG_CTL_STOP, a4) == 0, "STOP");
    usleep(100000);
    B->info(B->ctx, 1, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_STATE) == ACM_STATE_PAUSED && rd(out + 4 * ACM_IH_POSITION) == 0, "after STOP: paused at the start");
    a4[0] = 100;
    CHECK(B->control(B->ctx, 1, ACMG_CTL_RATE, a4) == ACM_E_BADARG, "RATE under 1/16: ACM_E_BADARG");
    a4[0] = 65536 * 4;
    CHECK(B->control(B->ctx, 1, ACMG_CTL_RATE, a4) == 0, "RATE 4");
    CHECK(B->control(B->ctx, 1, ACMG_CTL_SELECT, a4) == ACM_E_BADARG && B->control(B->ctx, 1, ACMG_CTL_TARGET, a4) == ACM_E_BADARG,
          "SELECT and TARGET: ACM_E_BADARG (not in the core)");
    a4[0] = 70000;
    CHECK(B->control(B->ctx, 1, ACMG_CTL_VOLUME, a4) == ACM_E_BADARG, "VOLUME over 65536: ACM_E_BADARG");
    a4[0] = 0;
    uint64_t tp = ms_now();
    CHECK(B->control(B->ctx, 1, ACMG_CTL_PLAY, a4) == 0 && wait_ev(h, ACMG_EV_END, &from, 3000, &e) >= 0, "at rate 4, to the end");
    uint64_t took = ms_now() - tp;
    CHECK(took >= 70, "rate 4: 360 ms of film in about 90 ms, not at once (%llu ms)", (unsigned long long)took);
    CHECK(B->audio(B->ctx, 1, out, 64, 1, 44100) == ACM_E_FORMAT && B->feed(B->ctx, 1, out, 4, 0) == ACM_E_BADARG,
          "AUDIO: ACM_E_FORMAT (step 6); FEED: ACM_E_BADARG (no feed sessions)");
    CHECK(B->open(B->ctx, 1, 0x201, d, srcd(d, "film.fake"), ACMG_ST_VIDEO, gtarget, NULL, 0, 0) == ACM_E_BADARG, "OPEN on a slot in use: ACM_E_BADARG");
    B->close(B->ctx, 1);
    CHECK(B->info(B->ctx, 1, out, sizeof out) == ACM_E_NOSUCH, "CLOSE: the slot is free at once");

    /* the slot again, at once; CLOSE while it plays */
    from = nev_now();
    h = opened(1, 2, "film.fake", gtarget, 0, &r);
    CHECK(r == ACM_PENDING && wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e) >= 0, "the slot opens again at once");
    CHECK(B->control(B->ctx, 1, ACMG_CTL_PLAY, a4) == 0, "PLAY, to be closed");
    usleep(60000);
    B->close(B->ctx, 1);
    CHECK(count_ev(h, ACMG_EV_FRAME, 0) == 0, "no FRAME events unless asked for");

    /* other targets and streams */
    uint8_t t2[32] = { 0 };
    be(t2, ACMG_OUT_RTG_SURFACE);
    CHECK(B->open(B->ctx, 2, 0x102, d, srcd(d, "film.fake"), ACMG_ST_VIDEO, t2, NULL, 0, 0) == ACM_E_FORMAT, "RTG_SURFACE: ACM_E_FORMAT (stage c)");
    be(t2, 9);
    CHECK(B->open(B->ctx, 2, 0x102, d, srcd(d, "film.fake"), ACMG_ST_VIDEO, t2, NULL, 0, 0) == ACM_E_BADARG, "an unknown target: ACM_E_BADARG");
    uint8_t atarget[32] = { 0 };
    be(atarget, ACMG_OUT_GUEST);
    from = nev_now();
    uint32_t an = srcd(d, "av.fake");
    r = B->open(B->ctx, 2, 0x102, d, an, ACMG_ST_VIDEO | ACMG_ST_AUDIO, gtarget, NULL, 0, 0);
    CHECK(r == ACM_PENDING && wait_ev(0x102, ACMG_EV_OPENED, &from, 5000, &e) >= 0, "A/V request without an audio target opens video only");
    B->info(B->ctx, 2, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_VIDEO) == 0 && rd(out + 4 * ACM_IH_AUDIO) == 0xffffffffu && rd(out + 4 * ACM_IH_AROUTE) == ACMG_ROUTE_NONE,
          "no audio target means no audio route");
    B->close(B->ctx, 2);
    from = nev_now();
    r = B->open(B->ctx, 2, 0x102, d, an, ACMG_ST_VIDEO | ACMG_ST_AUDIO, gtarget, atarget, 0, ACMG_OF_EVENTS_FRAME);
    CHECK(r == ACM_PENDING && wait_ev(0x102, ACMG_EV_OPENED, &from, 5000, &e) >= 0, "A/V OPENED");
    B->info(B->ctx, 2, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_VIDEO) == 0 && rd(out + 4 * ACM_IH_AUDIO) == 1 && rd(out + 4 * ACM_IH_AROUTE) == ACMG_ROUTE_GUEST,
          "INFO: video and guest audio; timing is separate from AUDIO");
    CHECK(rd(out + 4 * (ACM_INFO_HEAD_LONGS + ACM_INFO_STREAM_LONGS + ACM_IS_CHANNELS)) == 2,
          "INFO: guest audio is stereo");
    a4[0] = 0;
    CHECK(B->control(B->ctx, 2, ACMG_CTL_PLAY, a4) == 0, "A/V PLAY");
    int32_t abytes = ACM_E_AGAIN;
    for (int tries = 0; tries < 200 && abytes == ACM_E_AGAIN; tries++) { abytes = B->audio(B->ctx, 2, out, sizeof out, ACMG_SND_S16, 8000); if (abytes == ACM_E_AGAIN) usleep(5000); }
    CHECK(abytes == 512 && out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0 &&
          out[4] == 0 && out[5] == 1 && out[6] == 0 && out[7] == 1,
          "AUDIO: 512 paired S16BE stereo bytes (%d; %02x%02x/%02x%02x,%02x%02x/%02x%02x)",
          abytes, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
    usleep(100000);
    int burst = 0;
    while (B->audio(B->ctx, 2, out, sizeof out, ACMG_SND_S16, 8000) > 0) burst++;
    CHECK(burst >= 4, "AUDIO: independent clock queues a burst while video waits (%d blocks)", burst);
    CHECK(wait_ev(0x102, ACMG_EV_FRAME, &from, 3000, &e) >= 0, "A/V video frame event");
    pts = B->frame(B->ctx, 2, fb, sizeof fb, 64, ACMG_PIX_YUV420, 64, 48);
    CHECK(pts >= 0 && frame_ok(fb, 64, 48, 64, (uint32_t)pts / 40), "A/V FRAME: decoded video at PTS %d", pts);
    B->close(B->ctx, 2);
    from = nev_now();
    an = srcd(d, "av-tail.fake");
    r = B->open(B->ctx, 2, 0x102, d, an, ACMG_ST_VIDEO | ACMG_ST_AUDIO, gtarget, atarget, 0, 0);
    CHECK(r == ACM_PENDING && wait_ev(0x102, ACMG_EV_OPENED, &from, 5000, &e) >= 0 &&
          B->control(B->ctx, 2, ACMG_CTL_PLAY, a4) == 0,
          "A/V with a longer audio tail opens");
    usleep(250000);                 /* the 160 ms picture has ended; still within the audio grace */
    int samples = 0;
    uint64_t audio_deadline = ms_now() + 4000;
    for (;;) {
        int32_t ar = B->audio(B->ctx, 2, out, sizeof out, ACMG_SND_S16, 8000);
        if (ar > 0) { samples += ar / 4; continue; }
        if (ar == ACM_E_AGAIN && ms_now() < audio_deadline) { usleep(1000); continue; }
        CHECK(ar == ACM_E_EOF, "long audio tail ends cleanly (%d)", ar);
        break;
    }
    CHECK(samples == 6400, "audio after video EOF is not discarded (%d/6400 samples)", samples);
    B->close(B->ctx, 2);
    from = nev_now();
    an = srcd(d, "av-unread.fake");
    r = B->open(B->ctx, 2, 0x102, d, an, ACMG_ST_VIDEO | ACMG_ST_AUDIO, gtarget, atarget, 0, 0);
    CHECK(r == ACM_PENDING && wait_ev(0x102, ACMG_EV_OPENED, &from, 5000, &e) >= 0 &&
          B->control(B->ctx, 2, ACMG_CTL_PLAY, a4) == 0 && wait_ev(0x102, ACMG_EV_END, &from, 3000, &e) >= 0,
          "A/V END is independent of whether the guest reads audio");
    B->close(B->ctx, 2);
    uint8_t ahi[32] = { 0 }; be(ahi, ACMG_OUT_ACAHI);
    CHECK(B->open(B->ctx, 2, 0x102, d, an, ACMG_ST_AUDIO, NULL, ahi, 0, 0) == ACM_E_FORMAT, "direct ACAHI route is not claimed");
    CHECK(B->open(B->ctx, 2, 0x102, d, srcd(d, "missing.fake"), ACMG_ST_VIDEO, gtarget, NULL, 0, 0) == ACM_E_IO, "a missing source: ACM_E_IO at once");
    from = nev_now();
    h = opened(2, 3, "junk.bin", gtarget, 0, &r);
    CHECK(r == ACM_PENDING && wait_ev(h, ACMG_EV_ERROR, &from, 5000, &e) >= 0 && (int32_t)e.a == ACM_E_FORMAT, "OPEN of something else: ERROR ACM_E_FORMAT");
    B->info(B->ctx, 2, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_STATE) == ACM_STATE_ERROR && (int32_t)rd(out + 4 * ACM_IH_ERROR) == ACM_E_FORMAT && rd(out + 4 * ACM_IH_VIDEO) == 0xffffffffu,
          "INFO: the error, nothing open");
    B->close(B->ctx, 2);

    /* odd sizes, NV12 not needed: 34 x 18 at pitch 40 */
    from = nev_now();
    h = opened(2, 4, "short.fake", gtarget, ACMG_OF_EVENTS_FRAME, &r);
    CHECK(wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e) >= 0 && B->control(B->ctx, 2, ACMG_CTL_PLAY, a4) == 0 && wait_ev(h, ACMG_EV_FRAME, &from, 3000, &e) >= 0, "a 34x18 film plays");
    CHECK(B->frame(B->ctx, 2, fb, 40 * 18 + 2 * 20 * 9 - 1, 40, ACMG_PIX_YUV420, 34, 18) == ACM_E_TOOBIG, "FRAME into too little: ACM_E_TOOBIG");
    CHECK(B->frame(B->ctx, 2, fb, sizeof fb, 30, ACMG_PIX_YUV420, 34, 18) == ACM_E_BADARG, "FRAME at a pitch under the width: ACM_E_BADARG");
    pts = B->frame(B->ctx, 2, fb, sizeof fb, 40, ACMG_PIX_YUV420, 34, 18);
    CHECK(pts >= 0 && frame_ok(fb, 34, 18, 40, (uint32_t)pts / 20), "FRAME at pitch 40 of a 34x18 film (pts %d)", pts);
    B->close(B->ctx, 2);

    /* a worker that crashes */
    from = nev_now();
    h = opened(3, 5, "crash.fake", gtarget, 0, &r);
    CHECK(wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e) >= 0 && B->control(B->ctx, 3, ACMG_CTL_PLAY, a4) == 0, "the crashing film opens and plays");
    CHECK(wait_ev(h, ACMG_EV_ERROR, &from, 5000, &e) >= 0 && (int32_t)e.a == ACM_E_DECODER, "the worker crashes: ERROR ACM_E_DECODER");
    B->info(B->ctx, 3, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_STATE) == ACM_STATE_ERROR && (int32_t)rd(out + 4 * ACM_IH_ERROR) == ACM_E_DECODER, "INFO: ACM_E_DECODER");
    CHECK(B->control(B->ctx, 3, ACMG_CTL_PLAY, a4) == ACM_E_DECODER && B->frame(B->ctx, 3, fb, sizeof fb, 64, ACMG_PIX_YUV420, 0, 0) == ACM_E_DECODER,
          "and every call after: ACM_E_DECODER");
    char line[256] = { 0 };
    int32_t fl = B->fault(B->ctx, ACM_E_DECODER, (uint8_t *)line, sizeof line);
    CHECK(fl > 0 && strstr(line, "signal 11"), "FAULT: the line says why (%s)", line);
    CHECK(B->fault(B->ctx, ACM_E_DENIED, (uint8_t *)line, sizeof line) > 0 && strstr(line, "policy"), "FAULT: a deny's reason");
    CHECK(B->fault(B->ctx, ACM_E_EOF, (uint8_t *)line, sizeof line) == 0, "FAULT: else the broker's own line");
    B->close(B->ctx, 3);
    from = nev_now();
    h = opened(3, 6, "film.fake", gtarget, 0, &r);
    CHECK(wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e) >= 0 && B->control(B->ctx, 3, ACMG_CTL_PLAY, a4) == 0 && wait_ev(h, ACMG_EV_END, &from, 5000, &e) >= 0,
          "the next session plays");
    B->close(B->ctx, 3);

    /* PLANE with no acscreen: to ACM_FRAME, and RECT says so */
    be(vtarget, ACMG_OUT_PLANE); be(vtarget + 4, 10); be(vtarget + 8, 20); be(vtarget + 12, 320); be(vtarget + 16, 240);
    from = nev_now();
    h = opened(4, 7, "film.fake", vtarget, 0, &r);
    CHECK(wait_ev(h, ACMG_EV_RECT, &from, 5000, &e) >= 0 && e.a == ACM_RECT_NO_PLANE && e.b == ACMG_ROUTE_GUEST, "PLANE, no acscreen: RECT, route GUEST");
    wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e);
    B->info(B->ctx, 4, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_VROUTE) == ACMG_ROUTE_GUEST, "INFO: route GUEST");
    B->close(B->ctx, 4);

    /* PLANE with acscreen */
    vl = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", vsock);
    if (bind(vl, (struct sockaddr *)&sa, sizeof sa) || listen(vl, 1)) { perror("acscreen"); return 1; }
    pthread_t at; pthread_create(&at, NULL, acscreen, NULL);
    from = nev_now();
    h = opened(4, 8, "long.fake", vtarget, ACMG_OF_EVENTS_FRAME, &r);
    CHECK(wait_ev(h, ACMG_EV_OPENED, &from, 5000, &e) >= 0, "PLANE: OPENED");
    B->info(B->ctx, 4, out, sizeof out);
    CHECK(rd(out + 4 * ACM_IH_VROUTE) == ACMG_ROUTE_PLANE && count_ev(h, ACMG_EV_RECT, 0) == 0, "INFO: route PLANE");
    CHECK(B->control(B->ctx, 4, ACMG_CTL_PLAY, a4) == 0 && wait_ev(h, ACMG_EV_FRAME, &from, 3000, &e) >= 0, "frames shown on the plane (FRAME events)");
    usleep(150000);
    CHECK(vframes >= 2 && vcopies >= 2 && vfds_ok && vrects >= 1,
          "acscreen got software frames with the sealed pool fd, and the rectangle (%d frames, %d copies)", vframes, vcopies);
    int32_t onp = B->frame(B->ctx, 4, fb, sizeof fb, 64, ACMG_PIX_YUV420, 0, 0);
    CHECK(onp == ACM_E_AGAIN, "on the plane: nothing for ACM_FRAME (%d)", onp);
    a4[0] = 5 << 16 | 6; a4[1] = 100 << 16 | 80; a4[2] = 1;
    int rb = vrects;
    CHECK(B->control(B->ctx, 4, ACMG_CTL_SETRECT, a4) == 0, "SETRECT");
    usleep(100000);
    CHECK(vrects > rb, "SETRECT reaches acscreen");
    voff_req = 1;
    CHECK(wait_ev(h, ACMG_EV_RECT, &from, 3000, &e) >= 0 && e.a == ACV_W_NOT_SHOWN && e.b == ACMG_ROUTE_GUEST, "acscreen turns the plane off: RECT, route GUEST");
    usleep(150000);
    B->info(B->ctx, 4, out, sizeof out);
    pts = B->frame(B->ctx, 4, fb, sizeof fb, 64, ACMG_PIX_YUV420, 0, 0);
    CHECK(rd(out + 4 * ACM_IH_VROUTE) == ACMG_ROUTE_GUEST && (pts >= 0 ? frame_ok(fb, 64, 48, 64, (uint32_t)pts / 40) : pts == ACM_E_EOF || pts == ACM_E_AGAIN),
          "then the frames go to ACM_FRAME (%d)", pts);
    B->close(B->ctx, 4);

    /* nothing left behind */
    uint64_t tw = ms_now();
    while (workers() && ms_now() - tw < 5000) usleep(20000);
    CHECK(workers() == 0, "every worker ended and reaped (%d left)", workers());
    CHECK(sources > 10, "every source came through the host (%d)", sources);

    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -r -- '%s'", dir);
    if (system(cmd)) printf("note: %s left\n", dir);
    printf("acm_runtime_test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
