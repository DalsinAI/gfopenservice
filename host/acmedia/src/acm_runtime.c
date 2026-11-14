/*
 * libacmedia.so: ACMedia's backend for the runtime (designs/
 * ACMEDIA-DESIGN-20261110.md 2, 3.2, 6; M4K step 4). The runtime's broker
 * (gfosmotherboards amiga-native-bodies/native/acmedia_service.c) checks the
 * guest's ranges, owns the session handles and the event queue, and calls
 * this through struct acm_backend (acmedia_service.h); this side runs a
 * sandboxed acmedia-worker per session and per probe (acm_broker.c), and
 * puts the frames on acscreen's overlay plane (acm_plane.c) or hands them to
 * ACM_FRAME. It links no FFmpeg.
 *
 * Nothing here waits for a decoder on the emulation thread. ACM_PROBE and
 * ACM_OPEN take the source as an fd from the runtime (acm_host.source: the
 * runtime resolves the descriptor, this side never sees a path), start a
 * thread, and answer at once; the thread spawns the worker, and its outcome
 * comes as an event (PROBED, OPENED or ERROR). ACM_CONTROL queues the command
 * for the session's thread. ACM_FRAME copies the newest due frame out of the
 * worker's pool. ACM_CLOSE hands the session to its thread to end; the slot
 * may be opened again at once.
 *
 * What M4K step 4 needs, and what it leaves:
 *   - video and sound from the same worker session. ACM_AUDIO copies the
 *     next signed-16-bit big-endian stereo block and returns its byte count,
 *     as the guest ABI requires. The worker's audio PTS is retained internally
 *     to bound read-ahead, but ABI v1 has no per-block PTS field; INFO/events
 *     expose the due video position only.
 *     The Amiga-side consumer owns output pacing; ACAHI is a later direct-
 *     output route.
 *   - targets PLANE, GUEST and NONE. PLANE goes to acscreen's video socket
 *     ($AC_VIDEO_SOCK) as dma-bufs; with no acscreen, or while it says the
 *     plane is off, the frames go to ACM_FRAME instead (route GUEST) and a
 *     RECT event says so. The RTG surface the design falls to (stage c) is
 *     not here yet. RTG_SURFACE and CHIPSET are ACM_E_FORMAT.
 *   - ACM_FRAME gives YUV420 (planar: Y at pitch, then U and V at pitch / 2)
 *     at the decoded size; other formats or sizes are ACM_E_FORMAT (the
 *     conversions and scaling are stage c).
 *   - sources: whatever the runtime's source hook opens (ACM_SRC_ACFS);
 *     ACM_FEED is ACM_E_BADARG (no feed session in the core).
 *   - CONTROL: PLAY, PAUSE, STOP (paused at the start), SEEK, RATE (1/16 to
 *     16), VOLUME (kept), SETRECT, EVENTS; SELECT and TARGET are ACM_E_BADARG.
 *
 * The switches, read once at load:
 *   AC_ACMEDIA_WORKER  the worker (default: acmedia-worker beside this
 *                      library); missing or not executable: no backend, so
 *                      the ids stay off and akikoplus.device takes rung B
 *   AC_ACMEDIA_HW=0    no hardware decoders
 *   AC_ACMEDIA_DENY    tags the policy refuses, comma separated ("HEVC,AV1")
 *   AC_ACMEDIA_HWONLY  tags allowed only on a hardware decoder
 *   AC_VIDEO_SOCK      acscreen's video socket, for PLANE
 * Team 6, 12 Nov 2026. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "acm_broker.h"
#include "acm_codecs.h"
#include "acm_guest.h"
#include "acm_plane.h"
#include "acmedia_service.h"

extern const uint32_t acm_all_tags[];
extern const unsigned acm_all_tags_n;
uint32_t acm_tag_parse(const char *s);

#define RT_PROBES 16
#define RT_QUEUE 16
#define RT_AUDIO_QUEUE 8             /* about 170 ms at the worker's usual 1024/48 kHz blocks */
#define RT_AUDIO_IDLE_NS 500000000ull /* abandon only an unread tail, not active playback */
#define RT_AUDIO_AHEAD_MS 500         /* bound a buffering guest ahead of due video */
#define RT_NEXT_MS 10000              /* the watchdog on one frame */
#define RT_ASK_MS 2000
#define RT_POOL_MAX (768ull << 20)     /* the worker uses the same total cap */
#define RT_POOL_SLOTS_MAX 256u
#define RT_DIM_MAX 16384u
#define RT_PROBE_TTL_NS (60ull * 1000000000ull)

static void be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

/* the worker's paths (bits) as the guest numbers them */
static uint32_t gpath(uint32_t p) {
    return p & ACM_PATH_HW_V4L2REQ ? ACMG_PATH_HW_V4L2_REQUEST : p & ACM_PATH_HW_V4L2M2M ? ACMG_PATH_HW_V4L2_M2M
         : p & ACM_PATH_HW_VAAPI ? ACMG_PATH_HW_VAAPI : ACMG_PATH_SW;
}

enum { C_PLAY = 1, C_PAUSE, C_STOP, C_SEEK, C_RATE, C_RECT };
struct cmd { int k; int64_t a; uint32_t b; };

struct audio_block {
    struct acm_audio a;
    uint32_t bytes;
    uint8_t data[ACM_MSG_MAX - sizeof(struct acm_audio)];
};

struct rt;
struct sess {
    struct rt *rt;
    uint32_t slot, handle;
    int fd;
    uint32_t streams, vkind, akind, flags;
    struct acv_rect rect;
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    /* under mu */
    struct cmd q[RT_QUEUE];
    int qh, qn, closing;
    int state, opened;
    int32_t err;
    int64_t pos_ms;
    struct acm_opened op;
    int vroute;
    uint32_t vpath, rate, volume, evmask;
    int ready, taken;                /* the newest frame for ACM_FRAME, and whether it was read */
    struct acm_frame rf;
    struct audio_block aq[RT_AUDIO_QUEUE]; /* PCM runs on its own clock, not one block per video frame */
    int aqh, aqn;
    /* the thread's own */
    struct acm_worker w;
    struct acm_plane pl;
    int plane;
    int have;                        /* a frame from the worker not yet shown */
    struct acm_frame nf;
    int nfds, fds[ACM_MAX_FDS];
    uint64_t base_ns;
    int64_t base_pts;
    int based;
    int veof, aeof;
    uint64_t veof_ns, video_again_ns, audio_read_ns;
    int64_t audio_until_ms;
    int audio_drop;
};

struct probe {
    int used, done;
    uint32_t gen;
    int32_t err;
    int fd;
    uint64_t done_ns;
    struct acm_info info;
    struct rt *rt;
};

struct rt {
    const acm_host *host;
    char worker[4096], vsock[256];
    int hw;
    struct acm_hello hello;
    pthread_mutex_t mu;              /* the slots, the probes, the caps */
    struct sess *slots[ACM_SVC_SESSIONS + 1];
    struct probe probes[RT_PROBES];
    uint32_t probe_gen;
    int caps_state;                  /* 0 not asked, 1 asking, 2 known */
    struct acm_caps caps;
    char why[160];                   /* the newest worker failure, for ACM_FAULT */
};

static struct rt R = { .mu = PTHREAD_MUTEX_INITIALIZER };
static acm_backend B;

static void post(uint32_t h, uint32_t type, uint32_t a, uint32_t b) { if (R.host && R.host->post) R.host->post(h, type, a, b); }
static void note_why(const struct acm_worker *w) {
    if (!w->why[0]) return;
    pthread_mutex_lock(&R.mu);
    snprintf(R.why, sizeof R.why, "%s", w->why);
    pthread_mutex_unlock(&R.mu);
}

static int spawn(struct acm_worker *w, int fd) {
    int r = acm_spawn(w, R.worker, R.hw, R.hw ? 4096 : 1536, !getenv("ACM_VERBOSE"), &R.hello);
    if (r) { note_why(w); return r; }
    if (fd >= 0 && (r = acm_give_source(w, fd))) { note_why(w); acm_end(w); return r; }
    return 0;
}

/* ---- the policy and the caps ---- */

static void tags_env(const char *name, uint32_t *out, uint32_t *n) {
    const char *e = getenv(name);
    if (!e) return;
    char buf[512], *save = NULL;
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        uint32_t tag = acm_tag_parse(t);
        if (tag && *n < ACM_MAX_TAGS) out[(*n)++] = tag;
    }
}
static int tag_in(uint32_t t, const uint32_t *l, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (l[i] == t) return 1; return 0; }

static void *caps_main(void *a) {
    (void)a;
    struct acm_worker w;
    struct acm_caps c;
    memset(&c, 0, sizeof c);
    if (!spawn(&w, -1)) {
        uint32_t t, len;
        static uint8_t buf[sizeof(struct acm_caps) + 64];
        if (!acm_ask(&w, ACM_Q_CAPS, NULL, 0, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 10000) && t == ACM_A_CAPS && len >= sizeof c)
            memcpy(&c, buf, sizeof c);
        acm_end(&w);
    }
    pthread_mutex_lock(&R.mu);
    R.caps = c;
    R.caps_state = 2;
    pthread_mutex_unlock(&R.mu);
    return NULL;
}

static int32_t be_caps(void *ctx, uint32_t version, uint8_t *out, uint32_t len) {
    (void)ctx;
    if (version != ACMG_ABI_VERSION) return ACM_E_VERSION;
    if (len < ACM_CAPS_HEAD_LONGS * 4) return ACM_E_BADARG;
    pthread_mutex_lock(&R.mu);
    int known = R.caps_state == 2;
    struct acm_caps c = R.caps;
    pthread_mutex_unlock(&R.mu);
    memset(out, 0, len);
    be(out + 4 * ACM_CH_ABI, ACMG_ABI_VERSION);
    be(out + 4 * ACM_CH_ENTRIES, acm_all_tags_n);
    be(out + 4 * ACM_CH_SESSIONS, ACM_SVC_SESSIONS);
    be(out + 4 * ACM_CH_RANGE_MAX, ACMG_RANGE_MAX);
    be(out + 4 * ACM_CH_SMALL_MAX, ACMG_SMALL_MAX);
    be(out + 4 * ACM_CH_FLAGS, (R.hw ? ACM_CHF_HW : 0) | (R.hw && !known ? ACM_CHF_HWSCAN : 0));
    for (uint32_t i = 0; i < ACMG_CALLS; i++) be(out + 4 * (ACM_CH_CALLS + i), 1);
    uint32_t room = (len / 4 - ACM_CAPS_HEAD_LONGS) / ACM_CAPS_ENTRY_LONGS, n = 0;
    for (unsigned i = 0; i < acm_all_tags_n && n < room; i++, n++) {
        uint32_t tag = acm_all_tags[i], fl = ACM_CEF_BUILT, path = ACMG_PATH_SW, mw = 0, mh = 0, dep = 0;
        if (tag_in(tag, R.hello.allow, R.hello.nallow)) fl |= ACM_CEF_ALLOWED;
        else if (tag_in(tag, R.hello.hwonly, R.hello.nhwonly)) fl |= ACM_CEF_HWONLY;
        for (uint32_t k = 0; R.hw && known && k < c.n && k < ACM_MAX_HWCAPS; k++)
            if (c.c[k].tag == tag) { fl |= ACM_CEF_HW; path = gpath(c.c[k].path); mw = c.c[k].max_width; mh = c.c[k].max_height; dep = c.c[k].depths; break; }
        uint8_t *e = out + 4 * (ACM_CAPS_HEAD_LONGS + n * ACM_CAPS_ENTRY_LONGS);
        be(e + 4 * ACM_CE_TAG, tag); be(e + 4 * ACM_CE_FLAGS, fl); be(e + 4 * ACM_CE_PATH, path);
        be(e + 4 * ACM_CE_MAXW, mw); be(e + 4 * ACM_CE_MAXH, mh); be(e + 4 * ACM_CE_DEPTHS, dep);
    }
    return (int32_t)n;
}

/* ---- INFO and RESULT's block ---- */

static int32_t info_block(const struct acm_info *in, const uint32_t head[ACM_INFO_HEAD_LONGS], uint8_t *out, uint32_t len) {
    if (len < ACM_INFO_HEAD_LONGS * 4) return ACM_E_BADARG;
    memset(out, 0, len);
    for (int i = 0; i < ACM_INFO_HEAD_LONGS; i++) be(out + 4 * i, head[i]);
    be(out + 4 * ACM_IH_CONTAINER, in->container);
    be(out + 4 * ACM_IH_DURATION, (uint32_t)(in->duration_ms < 0 ? -1 : in->duration_ms > 0x7fffffff ? 0x7fffffff : in->duration_ms));
    uint32_t ns = in->nstreams > ACM_MAX_STREAMS ? ACM_MAX_STREAMS : in->nstreams;
    uint32_t room = (len / 4 - ACM_INFO_HEAD_LONGS) / ACM_INFO_STREAM_LONGS, n = ns < room ? ns : room;
    be(out + 4 * ACM_IH_STREAMS, ns);
    for (uint32_t i = 0; i < n; i++) {
        const struct acm_stream *s = &in->s[i];
        uint8_t *e = out + 4 * (ACM_INFO_HEAD_LONGS + i * ACM_INFO_STREAM_LONGS);
        uint32_t v[ACM_INFO_STREAM_LONGS] = { s->kind, s->tag, s->flags, s->width, s->height, s->bitdepth, s->rate_num, s->rate_den,
                                              s->sample_rate, s->channels,
                                              (uint32_t)(uint8_t)s->lang[0] << 24 | (uint32_t)(uint8_t)s->lang[1] << 16 | (uint32_t)(uint8_t)s->lang[2] << 8 | (uint8_t)s->lang[3], 0 };
        for (int k = 0; k < ACM_INFO_STREAM_LONGS; k++) be(e + 4 * k, v[k]);
    }
    return (int32_t)(4 * (ACM_INFO_HEAD_LONGS + n * ACM_INFO_STREAM_LONGS));
}

/* ---- probes ---- */

static void *probe_main(void *a) {
    struct probe *p = a;
    struct acm_worker w;
    struct acm_info info;
    memset(&info, 0, sizeof info);
    int r = spawn(&w, p->fd);
    if (!r) {
        uint32_t t, len;
        static __thread uint8_t buf[sizeof(struct acm_info) + 256];
        r = acm_ask(&w, ACM_Q_PROBE, NULL, 0, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 10000);
        if (!r && t == ACM_A_INFO && len >= sizeof info) memcpy(&info, buf, sizeof info);
        else if (!r) r = acm_err(t, buf, ACM_E_FORMAT);
        if (r) note_why(&w);
        acm_end(&w);
    }
    close(p->fd);
    pthread_mutex_lock(&R.mu);
    p->fd = -1; p->info = info; p->err = r; p->done_ns = now_ns(); p->done = 1;
    uint32_t ticket = p->gen << 8 | (uint32_t)(p - R.probes + 1);
    pthread_mutex_unlock(&R.mu);
    post(0, ACMG_EV_PROBED, ticket, 0);
    return NULL;
}

static int32_t be_probe(void *ctx, const uint8_t *src, uint32_t srclen, uint8_t *out, uint32_t len) {
    (void)ctx; (void)out; (void)len;
    int fd = -1;
    int32_t r = R.host->source ? R.host->source(src, srclen, &fd) : ACM_E_IO;
    if (r) return r;
    pthread_mutex_lock(&R.mu);
    struct probe *p = NULL;
    uint64_t now = now_ns();
    for (int i = 0; i < RT_PROBES; i++) {
        if (R.probes[i].used && R.probes[i].done && now - R.probes[i].done_ns >= RT_PROBE_TTL_NS)
            R.probes[i].used = 0;
        if (!p && !R.probes[i].used) p = &R.probes[i];
    }
    if (!p) { pthread_mutex_unlock(&R.mu); close(fd); return ACM_E_LIMIT; }
    R.probe_gen = (R.probe_gen + 1) & 0x7fffffu; if (!R.probe_gen) R.probe_gen = 1;
    memset(p, 0, sizeof *p);
    p->used = 1; p->gen = R.probe_gen; p->fd = fd;
    uint32_t ticket = p->gen << 8 | (uint32_t)(p - R.probes + 1);
    pthread_mutex_unlock(&R.mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, probe_main, p)) {
        pthread_attr_destroy(&at);
        close(fd);
        pthread_mutex_lock(&R.mu); p->used = 0; pthread_mutex_unlock(&R.mu);
        return ACM_E_LIMIT;
    }
    pthread_attr_destroy(&at);
    return (int32_t)ticket;
}

static int32_t be_result(void *ctx, uint32_t ticket, uint8_t *out, uint32_t len) {
    (void)ctx;
    uint32_t i = (ticket & 0xff) - 1;
    if (i >= RT_PROBES) return ACM_E_NOSUCH;
    pthread_mutex_lock(&R.mu);
    struct probe *p = &R.probes[i];
    if (!p->used || p->gen != ticket >> 8) { pthread_mutex_unlock(&R.mu); return ACM_E_NOSUCH; }
    if (!p->done) { pthread_mutex_unlock(&R.mu); return ACM_PENDING; }
    int32_t r = p->err;
    if (!r) {
        uint32_t head[ACM_INFO_HEAD_LONGS] = { 0 };
        head[ACM_IH_STATE] = ACM_STATE_PROBE;
        head[ACM_IH_VIDEO] = head[ACM_IH_AUDIO] = (uint32_t)-1;
        r = info_block(&p->info, head, out, len);
    }
    if (r != ACM_E_BADARG) p->used = 0;          /* read once; a short buffer may ask again */
    pthread_mutex_unlock(&R.mu);
    return r;
}

/* ---- sessions: the thread ---- */

static void drop_next(struct sess *s) {
    if (!s->have) return;
    for (int i = 0; i < s->nfds; i++) close(s->fds[i]);
    s->nfds = 0;
    acm_plane_release(&s->w, s->nf.slot);
    s->have = 0;
}
static void drop_ready(struct sess *s) {          /* mu not held */
    pthread_mutex_lock(&s->mu);
    int had = s->ready;
    uint32_t slot = s->rf.slot;
    s->ready = 0;
    pthread_mutex_unlock(&s->mu);
    if (had) acm_plane_release(&s->w, slot);
}
static void set_state(struct sess *s, int st, int32_t err) {
    pthread_mutex_lock(&s->mu);
    s->state = st;
    if (err) s->err = err;
    pthread_mutex_unlock(&s->mu);
}
static void plane_events(struct sess *s, int ms) {
    struct acm_plane_ev ev;
    while (s->plane) {
        int was = s->pl.on;
        int ty = acm_plane_pump(&s->pl, &s->w, ms, &ev);
        ms = 0;
        if (ty <= 0) {
            if (ty < 0) {                         /* acscreen went: the frames go to ACM_FRAME */
                acm_plane_close(&s->pl, &s->w, 0);
                s->plane = 0;
                pthread_mutex_lock(&s->mu); s->vroute = ACMG_ROUTE_GUEST; pthread_mutex_unlock(&s->mu);
                post(s->handle, ACMG_EV_RECT, ACV_W_HIDDEN, ACMG_ROUTE_GUEST);
            }
            return;
        }
        if (ev.composite) { for (int k = 0; k < ev.nfds; k++) close(ev.fds[k]); acm_plane_release(&s->w, ev.slot); }
        if (ty == ACV_SHOWN && (s->evmask & ACMG_EVM(ACMG_EV_FRAME))) post(s->handle, ACMG_EV_FRAME, (uint32_t)(ev.due_ns ? s->pos_ms : 0), 0);
        if (ty == ACV_STATE && was != s->pl.on) {
            pthread_mutex_lock(&s->mu); s->vroute = s->pl.on ? ACMG_ROUTE_PLANE : ACMG_ROUTE_GUEST; pthread_mutex_unlock(&s->mu);
            post(s->handle, ACMG_EV_RECT, s->pl.on ? ACV_W_OK : s->pl.why, s->pl.on ? ACMG_ROUTE_PLANE : ACMG_ROUTE_GUEST);
        }
    }
}

static int sess_open(struct sess *s) {
    int r = spawn(&s->w, s->fd);
    if (r) return r;
    struct acm_open o = {
        (s->streams & ACMG_ST_VIDEO) && s->vkind != ACMG_OUT_NONE ? -1 : -2,
        (s->streams & ACMG_ST_AUDIO) && s->akind != ACMG_OUT_NONE ? -1 : -2,
        0, 0
    };
    uint32_t t, len;
    static __thread uint8_t buf[sizeof(struct acm_opened) + 256];
    r = acm_ask(&s->w, ACM_Q_OPEN, &o, sizeof o, NULL, 0, &t, buf, sizeof buf, &len, NULL, NULL, 15000);
    if (!r && (t != ACM_A_OPENED || len < sizeof(struct acm_opened))) r = acm_err(t, buf, ACM_E_FORMAT);
    if (r) { note_why(&s->w); return r; }
    pthread_mutex_lock(&s->mu);
    memcpy(&s->op, buf, sizeof s->op);
    s->vpath = gpath(s->op.vpath);
    s->opened = 1;
    pthread_mutex_unlock(&s->mu);
    if (s->op.video >= 0 && (!s->op.width || !s->op.height || s->op.width > RT_DIM_MAX || s->op.height > RT_DIM_MAX))
        return ACM_E_DECODER;
    if (s->op.nslots) {
        uint64_t pool_size = (uint64_t)s->op.slot_size * s->op.nslots;
        if (!s->op.slot_size || s->op.nslots > RT_POOL_SLOTS_MAX || pool_size > RT_POOL_MAX || pool_size > SIZE_MAX)
            return ACM_E_TOOBIG;
        if ((r = acm_give_pool(&s->w, s->op.slot_size, s->op.nslots))) { note_why(&s->w); return r; }
    }
    return 0;
}

static void sess_plane(struct sess *s) {
    int route = s->vkind == ACMG_OUT_GUEST ? ACMG_ROUTE_GUEST : ACMG_ROUTE_NONE;
    if (s->vkind == ACMG_OUT_PLANE) {
        char why[160];
        route = ACMG_ROUTE_GUEST;
        if (s->op.video >= 0 && R.vsock[0] && !acm_plane_open(&s->pl, R.vsock, why, sizeof why)) {
            s->plane = 1;
            acm_plane_rect(&s->pl, &s->rect);
            if (s->pl.on) route = ACMG_ROUTE_PLANE;
        }
        if (route != ACMG_ROUTE_PLANE)
            post(s->handle, ACMG_EV_RECT, s->plane ? s->pl.why : ACM_RECT_NO_PLANE, ACMG_ROUTE_GUEST);
    }
    if (s->op.video < 0) route = ACMG_ROUTE_NONE;
    pthread_mutex_lock(&s->mu);
    s->vroute = route;
    pthread_mutex_unlock(&s->mu);
}

static void seek_to(struct sess *s, int64_t ms, uint32_t flags) {
    drop_next(s);
    drop_ready(s);
    uint32_t t, len;
    uint8_t buf[256];
    int r = acm_ask(&s->w, ACM_Q_SEEK, &(struct acm_seek){ ms, flags & ACMG_SEEK_EXACT, 0 }, sizeof(struct acm_seek), NULL, 0,
                    &t, buf, sizeof buf, &len, NULL, NULL, RT_ASK_MS);
    if (!r && t != ACM_A_OK) r = acm_err(t, buf, ACM_E_IO);
    s->based = 0;
    pthread_mutex_lock(&s->mu);
    s->aqh = s->aqn = 0;
    s->veof = s->aeof = 0;
    s->veof_ns = s->video_again_ns = s->audio_read_ns = 0;
    s->audio_until_ms = ms;
    s->audio_drop = 0;
    if (!r) { s->pos_ms = ms; if (s->state == ACM_STATE_ENDED) s->state = ACM_STATE_PAUSED; }
    int dead = s->w.dead;
    pthread_mutex_unlock(&s->mu);
    if (dead) { set_state(s, ACM_STATE_ERROR, ACM_E_DECODER); note_why(&s->w); post(s->handle, ACMG_EV_ERROR, (uint32_t)ACM_E_DECODER, 0); }
    else if (r == ACM_E_EOF) { set_state(s, ACM_STATE_ENDED, 0); post(s->handle, ACMG_EV_END, (uint32_t)s->pos_ms, 0); }
    else if (r) post(s->handle, ACMG_EV_ERROR, (uint32_t)r, 0);   /* the session stays as it was */
}

static void run_cmd(struct sess *s, struct cmd c) {
    switch (c.k) {
    case C_PLAY:
        pthread_mutex_lock(&s->mu);
        if (s->state == ACM_STATE_PAUSED) { s->state = ACM_STATE_PLAYING; s->based = 0; }
        pthread_mutex_unlock(&s->mu);
        break;
    case C_PAUSE:
        pthread_mutex_lock(&s->mu);
        if (s->state == ACM_STATE_PLAYING) s->state = ACM_STATE_PAUSED;
        pthread_mutex_unlock(&s->mu);
        break;
    case C_STOP:
        pthread_mutex_lock(&s->mu);
        if (s->state == ACM_STATE_PLAYING || s->state == ACM_STATE_ENDED) s->state = ACM_STATE_PAUSED;
        pthread_mutex_unlock(&s->mu);
        seek_to(s, 0, 0);
        break;
    case C_SEEK: seek_to(s, c.a, c.b); break;
    case C_RATE: s->based = 0; break;
    case C_RECT: if (s->plane) acm_plane_rect(&s->pl, &s->rect); break;
    }
}

/* Until t (CLOCK_MONOTONIC ns), still playing, nothing queued: 0 when it came, 1 when something else did.
 * With a plane, acscreen's packets are read meanwhile. */
static int step_audio(struct sess *s);

static int wait_until(struct sess *s, uint64_t t) {
    for (;;) {
        pthread_mutex_lock(&s->mu);
        int stop = s->closing || s->qn || s->state != ACM_STATE_PLAYING;
        uint64_t n = now_ns();
        if (stop || n >= t) { pthread_mutex_unlock(&s->mu); return stop; }
        uint64_t left = t - n, slice = left < 10000000ull ? left : 10000000ull;
        pthread_mutex_unlock(&s->mu);
        int ar = step_audio(s);      /* keep several audio blocks ready while the video waits for its PTS */
        if (ar) {
            note_why(&s->w);
            set_state(s, ACM_STATE_ERROR, ar);
            post(s->handle, ACMG_EV_ERROR, (uint32_t)ar, 0);
            return 1;
        }
        if (s->plane) {
            plane_events(s, (int)(slice / 1000000ull) + 1);
            continue;
        }
        pthread_mutex_lock(&s->mu);
        if (s->closing || s->qn || s->state != ACM_STATE_PLAYING) { pthread_mutex_unlock(&s->mu); return 1; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)slice;
        while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&s->cv, &s->mu, &ts);
        pthread_mutex_unlock(&s->mu);
    }
}

/* One frame further while playing: get it, wait for its time, show it. */
/* Fetch one audio block when the guest has consumed the previous one. Video
 * pacing never waits for the guest's audio copy. The worker's PTS stays with
 * the block internally and bounds audio read-ahead; ABI v1's ACM_AUDIO returns
 * only the copied byte count. */
static int step_audio(struct sess *s) {
    pthread_mutex_lock(&s->mu);
    /* Once video has ended, or its decoder is applying cross-stream
     * back-pressure, unread sound must not hold END/picture forever. An
     * active guest is allowed to play the whole tail; only an idle full
     * queue is discarded so the worker can be drained or video can move. */
    if ((s->veof || s->video_again_ns) && s->aqn == RT_AUDIO_QUEUE) {
        uint64_t edge = s->veof ? s->veof_ns : s->video_again_ns;
        uint64_t last = s->audio_read_ns > edge ? s->audio_read_ns : edge;
        if (s->audio_drop || now_ns() - last >= RT_AUDIO_IDLE_NS) {
            s->aqh = (s->aqh + 1) % RT_AUDIO_QUEUE;
            s->aqn--;
            s->audio_drop = 1;
        }
    }
    int ahead = s->op.video >= 0 && !s->veof && !s->video_again_ns && s->audio_until_ms > s->pos_ms + RT_AUDIO_AHEAD_MS;
    int need = s->op.audio >= 0 && s->aqn < RT_AUDIO_QUEUE && !s->aeof && !ahead;
    pthread_mutex_unlock(&s->mu);
    if (!need) return 0;
    static __thread uint8_t buf[ACM_MSG_MAX];
    uint32_t t, len;
    int r = acm_ask(&s->w, ACM_Q_NEXT, &(struct acm_next){ ACM_WANT_AUDIO, 0 }, sizeof(struct acm_next), NULL, 0,
                    &t, buf, sizeof buf, &len, NULL, NULL, RT_NEXT_MS);
    if (!r && t != ACM_A_AUDIO) r = acm_err(t, buf, ACM_E_DECODER);
    if (r == ACM_E_EOF) { pthread_mutex_lock(&s->mu); s->aeof = 1; pthread_mutex_unlock(&s->mu); return 0; }
    if (r == ACM_E_AGAIN) return 0;
    if (r) return r;
    if (len < sizeof(struct acm_audio)) return ACM_E_DECODER;
    struct acm_audio a;
    memcpy(&a, buf, sizeof a);
    uint64_t bytes = (uint64_t)a.frames * a.channels * 2;
    if (!a.frames || a.sample_rate != s->op.sample_rate || a.channels != 2 || bytes > sizeof s->aq[0].data || len != sizeof a + bytes)
        return ACM_E_DECODER;
    pthread_mutex_lock(&s->mu);
    struct audio_block *q = &s->aq[(s->aqh + s->aqn) % RT_AUDIO_QUEUE];
    q->a = a;
    q->bytes = (uint32_t)bytes;
    memcpy(q->data, buf + sizeof a, (size_t)bytes);
    s->aqn++;
    int64_t end = a.pts_ms >= 0 ? a.pts_ms : s->audio_until_ms;
    end += (int64_t)(((uint64_t)a.frames * 1000 + a.sample_rate - 1) / a.sample_rate);
    if (end > s->audio_until_ms) s->audio_until_ms = end;
    pthread_mutex_unlock(&s->mu);
    return 0;
}

static void finish_if_done(struct sess *s) {
    pthread_mutex_lock(&s->mu);
    int done = (s->op.video < 0 || s->veof) && (s->op.audio < 0 || s->aeof);
    int64_t pos = s->pos_ms;
    if (done) s->state = ACM_STATE_ENDED;
    pthread_mutex_unlock(&s->mu);
    if (done) post(s->handle, ACMG_EV_END, (uint32_t)pos, 0);
}

static void step(struct sess *s) {
    int ar = step_audio(s);
    if (ar) {
        note_why(&s->w);
        set_state(s, ACM_STATE_ERROR, ar);
        post(s->handle, ACMG_EV_ERROR, (uint32_t)ar, 0);
        return;
    }
    if (s->op.video < 0 || s->veof) { finish_if_done(s); if (s->state == ACM_STATE_PLAYING) usleep(1000); return; }
    if (!s->have) {
        uint32_t t, len;
        static __thread uint8_t buf[sizeof(struct acm_frame) + 256];
        uint32_t want = ACM_WANT_VIDEO | (s->plane && s->pl.on ? ACM_WANT_DMABUF : 0);
        s->nfds = 0;
        int r = acm_ask(&s->w, ACM_Q_NEXT, &(struct acm_next){ want, 0 }, sizeof(struct acm_next), NULL, 0, &t, buf, sizeof buf, &len,
                        s->fds, &s->nfds, RT_NEXT_MS);
        if (!r && t != ACM_A_FRAME) {
            for (int i = 0; i < s->nfds; i++) close(s->fds[i]);
            s->nfds = 0;
            r = acm_err(t, buf, ACM_E_DECODER);
        }
        if (r == ACM_E_AGAIN) {
            pthread_mutex_lock(&s->mu);
            if (!s->video_again_ns) s->video_again_ns = now_ns();
            pthread_mutex_unlock(&s->mu);
            plane_events(s, 10);
            return;
        }
        if (r == ACM_E_EOF) {
            pthread_mutex_lock(&s->mu); s->veof = 1; s->video_again_ns = 0; s->veof_ns = now_ns(); pthread_mutex_unlock(&s->mu);
            finish_if_done(s);
            return;
        }
        if (r) {
            note_why(&s->w);
            set_state(s, ACM_STATE_ERROR, r);
            post(s->handle, ACMG_EV_ERROR, (uint32_t)r, 0);
            return;
        }
        memcpy(&s->nf, buf, sizeof s->nf);
        s->have = 1;
        pthread_mutex_lock(&s->mu); s->video_again_ns = 0; pthread_mutex_unlock(&s->mu);
        if (s->nf.flags & ACM_FF_PATH) {
            pthread_mutex_lock(&s->mu); s->vpath = gpath(s->nf.path); pthread_mutex_unlock(&s->mu);
            post(s->handle, ACMG_EV_PATH, gpath(s->nf.path), 0);
        }
    }
    int64_t pts = s->nf.pts_ms < 0 ? s->pos_ms : s->nf.pts_ms;
    if (!s->based) { s->base_ns = now_ns(); s->base_pts = pts; s->based = 1; }
    uint32_t rate = s->rate ? s->rate : 65536;
    int64_t dt = pts - s->base_pts;
    uint64_t due = s->base_ns + (uint64_t)(dt > 0 ? dt : 0) * 1000000ull * 65536ull / rate;

    if (s->plane && s->pl.on) {
        /* acscreen shows it at its vblank; it gets it a frame's time ahead, and keeps one waiting */
        if (wait_until(s, due > 40000000ull ? due - 40000000ull : 0)) return;
        while (s->plane && acm_plane_inflight(&s->pl) >= 2) {
            pthread_mutex_lock(&s->mu);
            int closing = s->closing;
            pthread_mutex_unlock(&s->mu);
            if (closing) return;
            plane_events(s, 20);
        }
        uint64_t id;
        int r = s->nf.flags & ACM_FF_DMABUF ? acm_plane_send(&s->pl, &s->nf, s->fds, s->nfds, due, &id) :
                                               acm_plane_send_copy(&s->pl, &s->w, &s->nf, due, &id);
        if (r == 0) {
            s->have = 0; s->nfds = 0;
            pthread_mutex_lock(&s->mu); s->pos_ms = pts; pthread_mutex_unlock(&s->mu);
            plane_events(s, 0);
            return;
        }
        if (r < 0) { plane_events(s, 0); }
        /* A dma-buf frame cannot go to ACM_FRAME: give it back and ask for
         * the pool form. A software frame simply falls through to the guest. */
        if (s->nf.flags & ACM_FF_DMABUF) { drop_next(s); return; }
    }
    if (s->nf.flags & ACM_FF_DMABUF) { drop_next(s); return; }

    /* the guest's: wait for its time (a command wakes it), then it is the newest */
    if (wait_until(s, due)) return;
    pthread_mutex_lock(&s->mu);
    int had = s->ready;
    uint32_t old = s->rf.slot;
    s->rf = s->nf;
    s->ready = 1; s->taken = 0;
    s->pos_ms = pts;
    uint32_t mask = s->evmask;
    pthread_mutex_unlock(&s->mu);
    s->have = 0;
    for (int i = 0; i < s->nfds; i++) close(s->fds[i]);
    s->nfds = 0;
    if (had) acm_plane_release(&s->w, old);
    if (mask & ACMG_EVM(ACMG_EV_FRAME)) post(s->handle, ACMG_EV_FRAME, (uint32_t)pts, 0);
}

static void *sess_main(void *a) {
    struct sess *s = a;
    int r = sess_open(s);
    if (r) {
        set_state(s, ACM_STATE_ERROR, r);
        post(s->handle, ACMG_EV_ERROR, (uint32_t)r, 0);
    } else {
        sess_plane(s);
        set_state(s, ACM_STATE_PAUSED, 0);
        post(s->handle, ACMG_EV_OPENED, (uint32_t)(s->op.info.duration_ms < 0 ? -1 : s->op.info.duration_ms), s->vpath);
    }
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->closing && !s->qn && s->state != ACM_STATE_PLAYING) {
            if (s->plane) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&s->cv, &s->mu, &ts);
                break;
            }
            pthread_cond_wait(&s->cv, &s->mu);
        }
        if (s->closing) { pthread_mutex_unlock(&s->mu); break; }
        int has = s->qn > 0;
        struct cmd c = { 0, 0, 0 };
        if (has) { c = s->q[s->qh]; s->qh = (s->qh + 1) % RT_QUEUE; s->qn--; }
        int playing = s->state == ACM_STATE_PLAYING;
        pthread_mutex_unlock(&s->mu);
        if (s->state == ACM_STATE_ERROR) continue;   /* only the close is left */
        if (has) { run_cmd(s, c); continue; }
        if (playing) step(s);
        else plane_events(s, 0);
    }
    if (s->plane) acm_plane_close(&s->pl, &s->w, 500);
    drop_next(s);
    drop_ready(s);
    acm_end(&s->w);
    if (s->fd >= 0) close(s->fd);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s);
    return NULL;
}

/* ---- sessions: the calls ---- */

static struct sess *get(uint32_t slot) {
    if (!slot || slot > ACM_SVC_SESSIONS) return NULL;
    pthread_mutex_lock(&R.mu);
    struct sess *s = R.slots[slot];
    pthread_mutex_unlock(&R.mu);
    return s;
}

static int32_t be_open(void *ctx, uint32_t slot, uint32_t handle, const uint8_t *src, uint32_t srclen, uint32_t streams,
                       const uint8_t *video, const uint8_t *audio, uint32_t port, uint32_t flags) {
    (void)ctx; (void)port;
    if (!slot || slot > ACM_SVC_SESSIONS) return ACM_E_BADARG;
    if (!(streams & (ACMG_ST_VIDEO | ACMG_ST_AUDIO)) || (streams & ~(ACMG_ST_VIDEO | ACMG_ST_AUDIO))) return ACM_E_FORMAT;
    uint32_t vk = video ? rd(video) : ACMG_OUT_NONE;
    uint32_t ak = audio ? rd(audio) : ACMG_OUT_NONE;
    if (vk == ACMG_OUT_RTG_SURFACE || vk == ACMG_OUT_CHIPSET) return ACM_E_FORMAT;   /* stage c */
    if (vk != ACMG_OUT_NONE && vk != ACMG_OUT_PLANE && vk != ACMG_OUT_GUEST) return ACM_E_BADARG;
    if (ak != ACMG_OUT_NONE && ak != ACMG_OUT_GUEST) return ak == ACMG_OUT_ACAHI ? ACM_E_FORMAT : ACM_E_BADARG;
    struct sess *s = calloc(1, sizeof *s);
    if (!s) return ACM_E_LIMIT;
    s->rt = &R; s->slot = slot; s->handle = handle; s->streams = streams; s->vkind = vk; s->akind = ak; s->flags = flags; s->fd = -1;
    s->rate = 65536; s->volume = 65536; s->state = ACM_STATE_OPENING;
    s->evmask = flags & ACMG_OF_EVENTS_FRAME ? ACMG_EVM(ACMG_EV_FRAME) : 0;
    s->rect.space = ACV_SPACE_RTG; s->rect.visible = 1;
    if (vk == ACMG_OUT_PLANE) {
        s->rect.x = (int32_t)rd(video + 4); s->rect.y = (int32_t)rd(video + 8); s->rect.w = rd(video + 12); s->rect.h = rd(video + 16);
        if (!s->rect.w || !s->rect.h) s->rect.space = ACV_SPACE_FIT;
    }
    int32_t r = R.host->source ? R.host->source(src, srclen, &s->fd) : ACM_E_IO;
    if (r) { free(s); return r; }
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    pthread_mutex_lock(&R.mu);
    if (R.slots[slot]) { pthread_mutex_unlock(&R.mu); close(s->fd); free(s); return ACM_E_BADARG; }
    R.slots[slot] = s;
    pthread_mutex_unlock(&R.mu);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&s->th, &at, sess_main, s);
    pthread_attr_destroy(&at);
    if (e) {
        pthread_mutex_lock(&R.mu); R.slots[slot] = NULL; pthread_mutex_unlock(&R.mu);
        close(s->fd); free(s);
        return ACM_E_LIMIT;
    }
    return ACM_PENDING;
}

static void be_close(void *ctx, uint32_t slot) {
    (void)ctx;
    if (!slot || slot > ACM_SVC_SESSIONS) return;
    pthread_mutex_lock(&R.mu);
    struct sess *s = R.slots[slot];
    R.slots[slot] = NULL;                          /* the slot is free now; the thread ends the session */
    pthread_mutex_unlock(&R.mu);
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    s->closing = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static int32_t queue(struct sess *s, int k, int64_t a, uint32_t b) {
    if (s->state == ACM_STATE_ERROR) return ACM_E_DECODER;
    if (s->qn == RT_QUEUE) return ACM_E_LIMIT;
    s->q[(s->qh + s->qn++) % RT_QUEUE] = (struct cmd){ k, a, b };
    pthread_cond_signal(&s->cv);
    return 0;
}

static int32_t be_control(void *ctx, uint32_t slot, uint32_t cmd, const uint32_t a[4]) {
    (void)ctx;
    struct sess *s = get(slot);
    if (!s) return ACM_E_NOSUCH;
    int32_t r = 0;
    pthread_mutex_lock(&s->mu);
    switch (cmd) {
    case ACMG_CTL_PLAY: r = queue(s, C_PLAY, 0, 0); break;
    case ACMG_CTL_PAUSE: r = queue(s, C_PAUSE, 0, 0); break;
    case ACMG_CTL_STOP: r = queue(s, C_STOP, 0, 0); break;
    case ACMG_CTL_SEEK:
        if ((int32_t)a[0] < 0) { r = ACM_E_BADARG; break; }
        r = queue(s, C_SEEK, (int64_t)a[0], a[1]);
        break;
    case ACMG_CTL_RATE:
        if (a[0] < 4096 || a[0] > 16u * 65536) { r = ACM_E_BADARG; break; }
        s->rate = a[0];
        r = queue(s, C_RATE, 0, 0);
        break;
    case ACMG_CTL_VOLUME:
        if (a[0] > 65536) { r = ACM_E_BADARG; break; }
        s->volume = a[0];
        break;
    case ACMG_CTL_SETRECT:
        s->rect.x = (int16_t)(a[0] >> 16); s->rect.y = (int16_t)a[0];
        s->rect.w = a[1] >> 16; s->rect.h = a[1] & 0xffff;
        s->rect.visible = a[2] != 0;
        s->rect.space = s->rect.w && s->rect.h ? ACV_SPACE_RTG : ACV_SPACE_FIT;
        r = queue(s, C_RECT, 0, 0);
        break;
    case ACMG_CTL_EVENTS: s->evmask = a[0]; break;
    default: r = ACM_E_BADARG; break;             /* SELECT, TARGET: not in the core */
    }
    pthread_mutex_unlock(&s->mu);
    return r;
}

static int32_t be_info(void *ctx, uint32_t slot, uint8_t *out, uint32_t len) {
    (void)ctx;
    struct sess *s = get(slot);
    if (!s) return ACM_E_NOSUCH;
    pthread_mutex_lock(&s->mu);
    uint32_t head[ACM_INFO_HEAD_LONGS] = { 0 };
    head[ACM_IH_STATE] = (uint32_t)s->state;
    head[ACM_IH_POSITION] = (uint32_t)(s->pos_ms > 0x7fffffff ? 0x7fffffff : s->pos_ms);
    head[ACM_IH_VIDEO] = s->opened ? (uint32_t)s->op.video : (uint32_t)-1;
    head[ACM_IH_AUDIO] = s->opened ? (uint32_t)s->op.audio : (uint32_t)-1;
    head[ACM_IH_VPATH] = s->vpath;
    head[ACM_IH_VROUTE] = (uint32_t)s->vroute;
    head[ACM_IH_AROUTE] = s->opened && s->op.audio >= 0 ? ACMG_ROUTE_GUEST : ACMG_ROUTE_NONE;
    head[ACM_IH_SIZE] = (s->op.width & 0xffff) << 16 | (s->op.height & 0xffff);
    head[ACM_IH_ERROR] = (uint32_t)s->err;
    struct acm_info in = s->op.info;
    pthread_mutex_unlock(&s->mu);
    return info_block(&in, head, out, len);
}

static int source_frame_ok(const struct sess *s, const struct acm_frame *f) {
    if (f->slot >= s->w.nslots || !s->w.pool || !f->width || !f->height ||
        f->width > s->op.width || f->height > s->op.height)
        return 0;
    uint32_t cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    uint32_t np, rb[3], rows[3];
    if (f->pixfmt == ACM_PIX_YUV420P) {
        np = 3; rb[0] = f->width; rb[1] = rb[2] = cw; rows[0] = f->height; rows[1] = rows[2] = ch;
    } else if (f->pixfmt == ACM_PIX_NV12) {
        np = 2; rb[0] = f->width; rb[1] = 2 * cw; rows[0] = f->height; rows[1] = ch;
    } else return 0;
    if (f->nplanes < np) return 0;
    for (uint32_t p = 0; p < np; p++) {
        uint64_t end = (uint64_t)f->offset[p] + (uint64_t)(rows[p] - 1) * f->pitch[p] + rb[p];
        if (!f->pitch[p] || rb[p] > f->pitch[p] || end > s->w.slot_size) return 0;
    }
    return 1;
}

static int32_t be_frame(void *ctx, uint32_t slot, uint8_t *out, uint32_t len, uint32_t pitch, uint32_t format, uint32_t w, uint32_t h) {
    (void)ctx;
    struct sess *s = get(slot);
    if (!s) return ACM_E_NOSUCH;
    if (format != ACMG_PIX_YUV420) return ACM_E_FORMAT;          /* the conversions are stage c */
    pthread_mutex_lock(&s->mu);
    int32_t r;
    if (s->state == ACM_STATE_ERROR) r = ACM_E_DECODER;
    else if (s->opened && ((w && w != s->op.width) || (h && h != s->op.height))) r = ACM_E_FORMAT;   /* no scaling in the core */
    else if (s->vroute == ACMG_ROUTE_PLANE) r = s->state == ACM_STATE_ENDED ? ACM_E_EOF : ACM_E_AGAIN;   /* the frames are acscreen's */
    else if (!s->ready || s->taken) r = s->state == ACM_STATE_ENDED ? ACM_E_EOF : ACM_E_AGAIN;
    else {
        const struct acm_frame *f = &s->rf;
        if (!source_frame_ok(s, f)) r = ACM_E_DECODER;
        else {
            uint32_t fw = f->width, fh = f->height, cw = (fw + 1) / 2, chh = (fh + 1) / 2;
            if (!pitch) pitch = fw;
            if ((w && w != fw) || (h && h != fh)) r = ACM_E_FORMAT;   /* no scaling in the core */
            else if (pitch < fw || pitch / 2 < cw) r = ACM_E_BADARG;
            else if ((uint64_t)pitch * fh + 2ull * (pitch / 2) * chh > len) r = ACM_E_TOOBIG;
            else {
                const uint8_t *base = s->w.pool + (size_t)f->slot * s->w.slot_size;
                uint8_t *u = out + (size_t)pitch * fh, *v = u + (size_t)(pitch / 2) * chh;
                for (uint32_t y = 0; y < fh; y++) memcpy(out + (size_t)y * pitch, base + f->offset[0] + (size_t)y * f->pitch[0], fw);
                for (uint32_t y = 0; y < chh; y++) {
                    if (f->pixfmt == ACM_PIX_YUV420P) {
                        memcpy(u + (size_t)y * (pitch / 2), base + f->offset[1] + (size_t)y * f->pitch[1], cw);
                        memcpy(v + (size_t)y * (pitch / 2), base + f->offset[2] + (size_t)y * f->pitch[2], cw);
                    } else {
                        const uint8_t *row = base + f->offset[1] + (size_t)y * f->pitch[1];
                        for (uint32_t x = 0; x < cw; x++) { u[(size_t)y * (pitch / 2) + x] = row[2 * x]; v[(size_t)y * (pitch / 2) + x] = row[2 * x + 1]; }
                    }
                }
                s->taken = 1;
                r = (int32_t)(f->pts_ms < 0 ? 0 : f->pts_ms > 0x7fffffff ? 0x7fffffff : f->pts_ms);
            }
        }
    }
    pthread_mutex_unlock(&s->mu);
    return r;
}

static int32_t be_audio(void *ctx, uint32_t slot, uint8_t *out, uint32_t len, uint32_t format, uint32_t rate) {
    (void)ctx;
    struct sess *s = get(slot);
    if (!s) return ACM_E_NOSUCH;
    pthread_mutex_lock(&s->mu);
    int32_t r;
    if (s->state == ACM_STATE_ERROR) r = ACM_E_DECODER;
    else if (s->op.audio < 0 || format != ACMG_SND_S16 || (rate && rate != s->op.sample_rate)) r = ACM_E_FORMAT;
    else if (!s->aqn) r = s->state == ACM_STATE_ENDED ? ACM_E_EOF : ACM_E_AGAIN;
    else if (len < s->aq[s->aqh].bytes) r = ACM_E_TOOBIG;
    else {
        struct audio_block *q = &s->aq[s->aqh];
        uint32_t bytes = q->bytes;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        for (uint32_t i = 0; i < bytes; i += 2) {
            out[i] = q->data[i + 1];
            out[i + 1] = q->data[i];
        }
#else
        memcpy(out, q->data, bytes);
#endif
        s->aqh = (s->aqh + 1) % RT_AUDIO_QUEUE;
        s->aqn--;
        s->audio_read_ns = now_ns();
        s->audio_drop = 0;
        pthread_cond_signal(&s->cv);
        r = (int32_t)bytes;
    }
    pthread_mutex_unlock(&s->mu);
    return r;
}
static int32_t be_feed(void *ctx, uint32_t slot, const uint8_t *in, uint32_t len, uint32_t flags) {
    (void)ctx; (void)in; (void)len; (void)flags;
    return get(slot) ? ACM_E_BADARG : ACM_E_NOSUCH;              /* no feed sessions in the core */
}

static int32_t be_fault(void *ctx, int32_t code, uint8_t *out, uint32_t len) {
    (void)ctx;
    char line[256];
    if (code == ACM_E_DENIED) snprintf(line, sizeof line, "ACMedia: this host's codec policy refuses this codec (AC_ACMEDIA_DENY, AC_ACMEDIA_HWONLY)");
    else if (code == ACM_E_DECODER) {
        pthread_mutex_lock(&R.mu);
        if (R.why[0]) snprintf(line, sizeof line, "ACMedia: the decoder failed or stopped: %s", R.why);
        else line[0] = 0;
        pthread_mutex_unlock(&R.mu);
    } else line[0] = 0;
    if (!line[0]) return 0;                        /* the broker's own line */
    uint32_t l = (uint32_t)strlen(line) + 1;
    if (!len) return (int32_t)l;
    if (l > len) l = len;
    memcpy(out, line, l); out[l - 1] = 0;
    return (int32_t)l;
}

/* ---- the export ---- */

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int ok;
static void setup(void) {
    const char *w = getenv("AC_ACMEDIA_WORKER");
    if (w && *w) snprintf(R.worker, sizeof R.worker, "%s", w);
    else {
        Dl_info di;
        if (!dladdr((void *)setup, &di) || !di.dli_fname) return;
        snprintf(R.worker, sizeof R.worker, "%s", di.dli_fname);
        char *sl = strrchr(R.worker, '/');
        if (!sl) return;
        snprintf(sl + 1, sizeof R.worker - (size_t)(sl + 1 - R.worker), "acmedia-worker");
    }
    if (access(R.worker, X_OK)) { fprintf(stderr, "libacmedia: no worker at %s: ACMedia stays off\n", R.worker); return; }
    const char *hw = getenv("AC_ACMEDIA_HW");
    R.hw = !(hw && !strcmp(hw, "0"));
    const char *vs = getenv("AC_VIDEO_SOCK");
    if (vs) snprintf(R.vsock, sizeof R.vsock, "%s", vs);
    uint32_t deny[ACM_MAX_TAGS], nd = 0;
    memset(&R.hello, 0, sizeof R.hello);
    R.hello.version = ACM_PROTO_VERSION;
    R.hello.flags = R.hw ? ACM_HF_HW : 0;
    tags_env("AC_ACMEDIA_DENY", deny, &nd);
    tags_env("AC_ACMEDIA_HWONLY", R.hello.hwonly, &R.hello.nhwonly);
    for (unsigned i = 0; i < acm_all_tags_n && R.hello.nallow < ACM_MAX_TAGS; i++) {
        uint32_t t = acm_all_tags[i];
        if (!tag_in(t, deny, nd) && !tag_in(t, R.hello.hwonly, R.hello.nhwonly)) R.hello.allow[R.hello.nallow++] = t;
    }
    for (uint32_t i = 0; i < R.hello.nhwonly; ) {   /* denied wins over hardware-only */
        if (tag_in(R.hello.hwonly[i], deny, nd)) R.hello.hwonly[i] = R.hello.hwonly[--R.hello.nhwonly]; else i++;
    }
    B = (acm_backend){ ACM_SVC_ABI, &R, be_caps, be_probe, be_result, be_open, be_info, be_control, be_frame, be_audio, be_feed,
                       be_close, be_fault };
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    R.caps_state = 1;
    if (pthread_create(&th, &at, caps_main, NULL)) R.caps_state = 2;
    pthread_attr_destroy(&at);
    ok = 1;
}

__attribute__((visibility("default"))) const acm_backend *acmedia_backend_v1(const acm_host *host) {
    if (!host || host->abi != ACM_SVC_ABI || !host->post) return NULL;
    pthread_once(&once, setup);
    if (!ok) return NULL;
    R.host = host;
    return &B;
}
