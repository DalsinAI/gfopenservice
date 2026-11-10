/*
 * acmedia-worker: one decoding session in a process of its own (ACMedia,
 * amicradle-teams designs/ACMEDIA-DESIGN-20261110.md, sections 3 and 6).
 *
 *   acmedia-worker [--fd N] [--hw] [--mem MB] [--trap]
 *
 * Started by the broker with the socket on fd N (default 3) and an empty
 * environment. With --hw it opens the host's decoder devices first (VAAPI's
 * render node on x86-64; the DRM device for the V4L2 request and m2m
 * decoders on ARM64), then locks itself down (acm_sandbox.c) before it reads
 * its first message. From then on it has the fds it is given and nothing
 * else: the source (read only), the frame pool (a sealed memfd).
 *
 * Software frames are decoded straight into the pool's slots (a get_buffer2
 * of ours), so they cross to the broker with no copy; hardware frames go as
 * dma-buf fds, or are read back into the pool when the broker asks. A
 * hardware decoder that fails mid-stream is replaced by the software one at
 * the last keyframe, and the broker only sees ACM_FF_PATH on a frame.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team. FFmpeg is LGPL 2.1+ and linked
 * statically; see README.md for the sources and how to relink.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "acm_proto.h"
#include "acm_msg.h"
#include "acm_codecs.h"
#include "acm_ffmap.h"
#include "acm_sandbox.h"
#include "acm_caps.h"

#define HW_HELD 32
#define HW_HANDLE 0x80000000u

static int sock = 3;
static uint8_t rx[ACM_MSG_MAX];

/* policy */
static struct acm_hello hello;
static int hello_seen;

/* hardware devices, opened before lock-down */
static AVBufferRef *hw_vaapi, *hw_drm;
static uint32_t hw_paths;

/* the source */
static int src_fd = -1;
static int64_t src_size = -1;
static AVIOContext *avio;
static AVFormatContext *fmt;

/* the session */
struct dec {
    AVCodecContext *c;
    int idx;
    uint32_t path;
    enum AVPixelFormat hw_fmt;
    int hw_failed;               /* get_format found no hardware format */
    int64_t last_pts;            /* in the stream's time base, of the last frame given out */
    int64_t skip_until;          /* after a fallback: drop frames up to this pts */
    int path_changed;
    int drained;
};
static struct dec V = { .idx = -1 }, A = { .idx = -1 };
static AVFrame *pending;          /* the pre-rolled first video frame */
static SwrContext *swr;
static uint32_t a_rate, a_ch;
static AVFrame *aq[64];           /* decoded audio not yet asked for */
static int aqn;

/* the pool */
static uint8_t *pool;
static uint32_t slot_size, nslots;
static uint8_t slot_busy[256];
static AVFrame *slot_frame[256];  /* a zero-copy frame the broker holds */
static uint8_t slot_given[256];   /* the broker holds this slot (zero-copy or copied) */
static pthread_mutex_t slot_mx = PTHREAD_MUTEX_INITIALIZER;
static enum AVPixelFormat pool_fmt = AV_PIX_FMT_NONE;
static AVFrame *hw_held[HW_HELD];
static int copy_only;
static unsigned frames_out;
static int last_ff;               /* FFmpeg's last error, for the error's text */

#ifdef ACM_TEST_HOOKS
static uint32_t hook, hook_arg;
#endif

static void reply_error(int code, uint32_t detail, const char *text)
{
    struct acm_error e = { code, detail, { 0 } };
    if (text) snprintf(e.text, sizeof e.text, "%s", text);
    acm_send(sock, ACM_A_ERROR, &e, sizeof e, NULL, 0, NULL, 0);
}
static void reply_ok(const void *p, uint32_t n) { acm_send(sock, ACM_A_OK, p, n, NULL, 0, NULL, 0); }

/* ---- the source, through our own AVIOContext: no protocol is built in ---- */

static int src_read(void *o, uint8_t *buf, int n)
{
    (void)o;
    ssize_t r;
    do r = read(src_fd, buf, (size_t)n); while (r < 0 && errno == EINTR);
    if (r < 0) return AVERROR(EIO);
    return r == 0 ? AVERROR_EOF : (int)r;
}
static int64_t src_seek(void *o, int64_t off, int whence)
{
    (void)o;
    if (whence == AVSEEK_SIZE) return src_size >= 0 ? src_size : AVERROR(ENOSYS);
    off_t r = lseek(src_fd, (off_t)off, whence & ~AVSEEK_FORCE);
    return r < 0 ? AVERROR(errno) : (int64_t)r;
}

/* ---- policy ---- */

static int tag_in(uint32_t t, const uint32_t *l, uint32_t n)
{
    for (uint32_t i = 0; i < n && i < ACM_MAX_TAGS; i++) if (l[i] == t) return 1;
    return 0;
}
/* 0 allowed (*hwonly set when only on hardware), else an ACM_E_ */
static int policy(uint32_t tag, int *hwonly)
{
    *hwonly = 0;
    if (!tag) return ACM_E_NOCODEC;
    if (tag_in(tag, hello.allow, hello.nallow)) return 0;
    if (tag_in(tag, hello.hwonly, hello.nhwonly)) { *hwonly = 1; return 0; }
    return ACM_E_DENIED;
}

/* Which hardware path would take this codec here (0: none) */
static uint32_t hw_path_for(enum AVCodecID id, enum AVPixelFormat *hwfmt, const AVCodec **codec)
{
    if (!(hello.flags & ACM_HF_HW)) return 0;
    if (hw_vaapi) {
        const AVCodec *c = avcodec_find_decoder(id);
        for (int i = 0; c; i++) {
            const AVCodecHWConfig *h = avcodec_get_hw_config(c, i);
            if (!h) break;
            if (h->device_type == AV_HWDEVICE_TYPE_VAAPI && (h->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                *hwfmt = h->pix_fmt; *codec = c;
                return ACM_PATH_HW_VAAPI;
            }
        }
    }
    if (hw_drm && id == AV_CODEC_ID_HEVC && (hw_paths & ACM_PATH_HW_V4L2REQ)) {
        const AVCodec *c = avcodec_find_decoder(id);
        for (int i = 0; c; i++) {
            const AVCodecHWConfig *h = avcodec_get_hw_config(c, i);
            if (!h) break;
            if (h->device_type == AV_HWDEVICE_TYPE_DRM && (h->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                *hwfmt = h->pix_fmt; *codec = c;
                return ACM_PATH_HW_V4L2REQ;
            }
        }
    }
    if (id == AV_CODEC_ID_H264 && (hw_paths & ACM_PATH_HW_V4L2M2M)) {
        const AVCodec *c = avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (c) { *hwfmt = AV_PIX_FMT_DRM_PRIME; *codec = c; return ACM_PATH_HW_V4L2M2M; }
    }
    return 0;
}

/* ---- probe ---- */

/* A stream's tag: its codec's, except a tracker module (libopenmpt hands
 * out rendered PCM), which is 'MOD ' so the policy can name it. */
static uint32_t stream_tag(unsigned i)
{
    if (!strcmp(fmt->iformat->name, "libopenmpt")) return ACM_T_MODULE;
    return acm_tag_of(fmt->streams[i]->codecpar->codec_id);
}

static int open_demuxer(void)
{
    if (fmt) return 0;
    if (src_fd < 0) return ACM_E_BADARG;
    uint8_t *b = av_malloc(65536);
    if (!b) return ACM_E_LIMIT;
    avio = avio_alloc_context(b, 65536, 0, NULL, src_read, NULL, src_size >= 0 ? src_seek : NULL);
    if (!avio) { av_free(b); return ACM_E_LIMIT; }
    avio->seekable = src_size >= 0 ? AVIO_SEEKABLE_NORMAL : 0;
    fmt = avformat_alloc_context();
    if (!fmt) return ACM_E_LIMIT;
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->probesize = 8 << 20;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) { fmt = NULL; return ACM_E_FORMAT; }
    if (avformat_find_stream_info(fmt, NULL) < 0) return ACM_E_FORMAT;
    return 0;
}

static void fill_info(struct acm_info *in)
{
    memset(in, 0, sizeof *in);
    in->container = acm_container_tag(fmt->iformat->name);
    snprintf(in->container_name, sizeof in->container_name, "%s", fmt->iformat->name);
    in->duration_ms = fmt->duration > 0 ? fmt->duration / 1000 : -1;
    for (unsigned i = 0; i < fmt->nb_streams && in->nstreams < ACM_MAX_STREAMS; i++) {
        const AVCodecParameters *p = fmt->streams[i]->codecpar;
        struct acm_stream *s = &in->s[in->nstreams++];
        s->kind = p->codec_type == AVMEDIA_TYPE_VIDEO ? ACM_KIND_VIDEO
                : p->codec_type == AVMEDIA_TYPE_AUDIO ? ACM_KIND_AUDIO
                : p->codec_type == AVMEDIA_TYPE_SUBTITLE ? ACM_KIND_SUB : 0;
        s->tag = stream_tag(i);
        int hwonly;
        int pol = policy(s->tag, &hwonly);
        if (pol == ACM_E_DENIED) s->flags |= ACM_SF_DENIED;
        if (!s->tag || !avcodec_find_decoder(p->codec_id)) s->flags |= ACM_SF_NOCODEC;
        (void)0;
        enum AVPixelFormat hf; const AVCodec *hc;
        if (s->kind == ACM_KIND_VIDEO && hw_path_for(p->codec_id, &hf, &hc)) s->flags |= ACM_SF_HWOK;
        s->width = (uint32_t)p->width;
        s->height = (uint32_t)p->height;
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(p->format);
        s->bitdepth = d ? (uint32_t)d->comp[0].depth : 0;
        AVRational fr = fmt->streams[i]->avg_frame_rate;
        s->rate_num = (uint32_t)(fr.num > 0 ? fr.num : 0);
        s->rate_den = (uint32_t)(fr.den > 0 ? fr.den : 0);
        s->sample_rate = (uint32_t)(p->sample_rate > 0 ? p->sample_rate : 0);
        s->channels = (uint32_t)(p->ch_layout.nb_channels > 0 ? p->ch_layout.nb_channels : 0);
        const AVDictionaryEntry *l = av_dict_get(fmt->streams[i]->metadata, "language", NULL, 0);
        if (l) memcpy(s->lang, l->value, strnlen(l->value, 3));
    }
}

static void do_probe(void)
{
    int r = open_demuxer();
    if (r) { reply_error(r, 0, "not a format this build reads"); return; }
    struct acm_info in;
    fill_info(&in);
    acm_send(sock, ACM_A_INFO, &in, sizeof in, NULL, 0, NULL, 0);
}

/* ---- the pool: software decoders write straight into it ---- */

static int layout(enum AVPixelFormat f, int w, int h, uint32_t off[4], uint32_t pitch[4],
                  uint32_t rowbytes[4], uint32_t rows[4], uint32_t *total)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f);
    int ls[4];
    if (!d || av_image_fill_linesizes(ls, f, w) < 0) return -1;
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) {
        off[i] = pitch[i] = rowbytes[i] = rows[i] = 0;
        int ph = (i == 1 || i == 2) ? -((-h) >> d->log2_chroma_h) : h;
        if ((d->flags & AV_PIX_FMT_FLAG_PAL) && i == 1) { ls[i] = 1024; ph = 1; }
        if (ls[i] <= 0) continue;
        pitch[i] = (uint32_t)FFALIGN(ls[i], 64);
        rowbytes[i] = (uint32_t)ls[i];
        rows[i] = (uint32_t)ph;
        off[i] = o;
        o += pitch[i] * (uint32_t)ph + 1024;                  /* decoders may write a little past a plane */
        o = (uint32_t)FFALIGN(o, 64);
    }
    if (d->flags & AV_PIX_FMT_FLAG_PAL) { /* the palette is plane 1, already laid out */ }
    *total = o;
    return 0;
}

/* A free slot, or -1. reserve: leave that many free (the decoder never takes
 * the last ones, which copies need). */
static int take_slot_r(uint32_t reserve)
{
    pthread_mutex_lock(&slot_mx);
    int s = -1;
    uint32_t nfree = 0;
    for (uint32_t i = 0; i < nslots; i++) if (!slot_busy[i]) nfree++;
    if (nfree > reserve)
        for (uint32_t i = 0; i < nslots; i++) if (!slot_busy[i]) { slot_busy[i] = 1; s = (int)i; break; }
    pthread_mutex_unlock(&slot_mx);
    return s;
}
static int take_slot(void) { return take_slot_r(0); }
static void free_slot(void *opaque, uint8_t *data)
{
    (void)data;
    pthread_mutex_lock(&slot_mx);
    slot_busy[(uintptr_t)opaque] = 0;
    pthread_mutex_unlock(&slot_mx);
}

static int get_buffer(AVCodecContext *c, AVFrame *f, int flags)
{
    if (!pool || copy_only || !(c->codec->capabilities & AV_CODEC_CAP_DR1))
        return avcodec_default_get_buffer2(c, f, flags);
    int w = f->width, h = f->height, la[AV_NUM_DATA_POINTERS];
    avcodec_align_dimensions2(c, &w, &h, la);
    uint32_t off[4], pitch[4], rb[4], rows[4], total;
    if (layout(f->format, w, h, off, pitch, rb, rows, &total) || total > slot_size)
        return avcodec_default_get_buffer2(c, f, flags);
    int s = take_slot_r(4);
    if (s < 0) return avcodec_default_get_buffer2(c, f, flags);
    uint8_t *base = pool + (size_t)s * slot_size;
    f->buf[0] = av_buffer_create(base, slot_size, free_slot, (void *)(uintptr_t)s, 0);
    if (!f->buf[0]) { free_slot((void *)(uintptr_t)s, NULL); return AVERROR(ENOMEM); }
    for (int i = 0; i < 4; i++) {
        f->data[i] = pitch[i] ? base + off[i] : NULL;
        f->linesize[i] = (int)pitch[i];
    }
    f->extended_data = f->data;
    return 0;
}

static enum AVPixelFormat get_format(AVCodecContext *c, const enum AVPixelFormat *f)
{
    struct dec *d = c->opaque;
    for (const enum AVPixelFormat *p = f; *p != AV_PIX_FMT_NONE; p++)
        if (d->hw_fmt != AV_PIX_FMT_NONE && *p == d->hw_fmt) return *p;
    d->hw_failed = 1;
    for (const enum AVPixelFormat *p = f; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *x = av_pix_fmt_desc_get(*p);
        if (x && !(x->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

/* ---- decoders ---- */

/* FFmpeg's errors (AVERROR(errno) are small negatives too, so they must
 * never pass as ours) to ACMedia's. */
static int ff2acm(int r)
{
    if (r >= 0) return r;
    if (r == AVERROR_EOF) return ACM_E_EOF;
    if (r == AVERROR(ENOMEM)) return ACM_E_LIMIT;
    if (r == AVERROR_INVALIDDATA) return ACM_E_FORMAT;
    if (r == AVERROR(EIO)) return ACM_E_IO;
    if (r == AVERROR_DECODER_NOT_FOUND || r == AVERROR_PATCHWELCOME) return ACM_E_NOCODEC;
    return ACM_E_DECODER;
}

static void close_dec(struct dec *d)
{
    if (d->c) avcodec_free_context(&d->c);
    d->path = ACM_PATH_NONE;
    d->hw_fmt = AV_PIX_FMT_NONE;
    d->hw_failed = 0;
    d->drained = 0;
}

/* Opens stream idx's decoder: hardware first when allowed and present, else
 * software. hwonly: the policy allows this codec on hardware only. */
static int open_dec(struct dec *d, int idx, int want_hw, int hwonly)
{
    const AVCodecParameters *p = fmt->streams[idx]->codecpar;
    const AVCodec *codec = NULL;
    enum AVPixelFormat hwfmt = AV_PIX_FMT_NONE;
    uint32_t path = want_hw ? hw_path_for(p->codec_id, &hwfmt, &codec) : 0;
    if (!path) {
        if (hwonly) return ACM_E_DENIED;
        codec = avcodec_find_decoder(p->codec_id);
        if (!codec) return ACM_E_NOCODEC;
    }
    d->c = avcodec_alloc_context3(codec);
    if (!d->c) return ACM_E_LIMIT;
    avcodec_parameters_to_context(d->c, p);
    d->c->opaque = d;
    d->c->pkt_timebase = fmt->streams[idx]->time_base;
    d->idx = idx;
    d->hw_fmt = AV_PIX_FMT_NONE;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        d->c->get_buffer2 = get_buffer;
        int nc = (int)sysconf(_SC_NPROCESSORS_ONLN);
        d->c->thread_count = nc > 8 ? 8 : nc > 0 ? nc : 1;     /* each thread holds frames: keep it bounded */
        if (path == ACM_PATH_HW_VAAPI || path == ACM_PATH_HW_V4L2REQ) {
            d->c->hw_device_ctx = av_buffer_ref(path == ACM_PATH_HW_VAAPI ? hw_vaapi : hw_drm);
            d->c->get_format = get_format;
            d->c->extra_hw_frames = 8;                          /* the frames the broker may hold */
            d->hw_fmt = hwfmt;
        }
    }
    if (avcodec_open2(d->c, codec, NULL) < 0) {
        close_dec(d);
        if (path) return open_dec(d, idx, 0, hwonly);           /* the device would not open: software */
        return ACM_E_NOCODEC;
    }
    d->path = path ? path : ACM_PATH_SW;
    return 0;
}

/* Reads packets until dec has a frame (0), or the end (ACM_E_EOF), or an error. */
static int feed(struct dec *want);

static void queue_audio(AVFrame *f)
{
    if (aqn == 64) { av_frame_free(&aq[0]); memmove(aq, aq + 1, sizeof(aq[0]) * 63); aqn--; }
    aq[aqn++] = f;
}

static int receive(struct dec *d, AVFrame *out)
{
    for (;;) {
        int r = avcodec_receive_frame(d->c, out);
        if (r == 0) return 0;
        if (r == AVERROR_EOF) return ACM_E_EOF;
        if (r != AVERROR(EAGAIN)) { last_ff = r; return ff2acm(r); }
        r = feed(d);
        if (r) return r;
    }
}

static int feed(struct dec *want)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return ACM_E_LIMIT;
    int r = 0;
    for (;;) {
        int e = av_read_frame(fmt, pkt);
        if (e == AVERROR_EOF || (e < 0 && avio_feof(fmt->pb))) {
            if (!want->drained) { avcodec_send_packet(want->c, NULL); want->drained = 1; }
            break;
        }
        if (e < 0) { r = ACM_E_IO; break; }
        struct dec *to = pkt->stream_index == V.idx && V.c ? &V : pkt->stream_index == A.idx && A.c ? &A : NULL;
        if (to) {
            int s = avcodec_send_packet(to->c, pkt);
            if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_INVALIDDATA && to == want) { av_packet_unref(pkt); last_ff = s; r = ff2acm(s); break; }
            if (to == &A && want != &A) {                    /* keep the audio decoder drained */
                AVFrame *f = av_frame_alloc();
                while (f && avcodec_receive_frame(A.c, f) == 0) { queue_audio(f); f = av_frame_alloc(); }
                av_frame_free(&f);
            }
        }
        av_packet_unref(pkt);
        if (to == want) break;
    }
    av_packet_free(&pkt);
    return r;
}

/* The hardware decoder failed: the software one takes over at the last keyframe. */
static int fallback_to_sw(void)
{
    int idx = V.idx;
    int64_t last = V.last_pts;
    char why[AV_ERROR_MAX_STRING_SIZE] = "a test";
    if (last_ff) av_strerror(last_ff, why, sizeof why);
    fprintf(stderr, "acmedia-worker: the hardware decoder failed (%s) after %u frames; software takes over\n", why, frames_out);
    close_dec(&V);
    int r = open_dec(&V, idx, 0, 0);
    if (r) return r;
    if (last != AV_NOPTS_VALUE) {
        av_seek_frame(fmt, idx, last, AVSEEK_FLAG_BACKWARD);
        if (A.c) avcodec_flush_buffers(A.c);
        V.skip_until = last;
    }
    V.path_changed = 1;
    return 0;
}

static int next_video(AVFrame *f)
{
    if (pending) { av_frame_move_ref(f, pending); av_frame_free(&pending); return 0; }
    for (;;) {
        int r = receive(&V, f);
        int failed = r < 0 && r != ACM_E_EOF && V.path != ACM_PATH_SW;
#ifdef ACM_TEST_HOOKS
        if (r == 0 && hook == ACM_HOOK_FAIL_HW && V.path != ACM_PATH_SW && frames_out >= hook_arg) {
            av_frame_unref(f);
            failed = 1;
            hook = 0;
        }
#endif
        if (r == 0 && V.path != ACM_PATH_SW && V.hw_failed && f->format != V.hw_fmt) {
            V.path = ACM_PATH_SW;                               /* hwaccel refused at the first frame */
            V.path_changed = 1;
        }
        if (failed) {
            int e = fallback_to_sw();
            if (e) return e;
            continue;
        }
        if (r) return r;
        if (V.skip_until != AV_NOPTS_VALUE && f->pts != AV_NOPTS_VALUE && f->pts <= V.skip_until) {
            av_frame_unref(f);
            continue;
        }
        V.skip_until = AV_NOPTS_VALUE;
        return 0;
    }
}

/* ---- OPEN ---- */

static uint32_t acm_pix(enum AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: return ACM_PIX_YUV420P;
    case AV_PIX_FMT_NV12: return ACM_PIX_NV12;
    case AV_PIX_FMT_YUV420P10LE: return ACM_PIX_YUV420P10;
    case AV_PIX_FMT_P010LE: return ACM_PIX_P010;
    default: return ACM_PIX_OTHER;
    }
}

static void do_open(const struct acm_open *o)
{
    if (V.c || A.c) { reply_error(ACM_E_BADARG, 0, "already open"); return; }
    int r = open_demuxer();
    if (r) { reply_error(r, 0, "not a format this build reads"); return; }
    struct acm_opened op;
    memset(&op, 0, sizeof op);
    fill_info(&op.info);
    int vi = o->video == -2 ? -1 : o->video >= 0 ? o->video : av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = o->audio == -2 ? -1 : o->audio >= 0 ? o->audio : av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    if (vi >= (int)fmt->nb_streams || ai >= (int)fmt->nb_streams) { reply_error(ACM_E_BADARG, 0, "no such stream"); return; }
    if (vi >= 0 && fmt->streams[vi]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) vi = -1;
    if (ai >= 0 && fmt->streams[ai]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) ai = -1;
    if (vi < 0 && ai < 0) { reply_error(ACM_E_FORMAT, 0, "no video or audio stream"); return; }
    V.last_pts = A.last_pts = V.skip_until = A.skip_until = AV_NOPTS_VALUE;
    for (int k = 0; k < 2; k++) {
        int idx = k ? ai : vi;
        if (idx < 0) continue;
        int hwonly;
        uint32_t tag = stream_tag((unsigned)idx);
        int pol = policy(tag, &hwonly);
        if (pol) {                                              /* never opened: the policy said no */
            close_dec(&V); close_dec(&A);
            reply_error(pol, tag, pol == ACM_E_DENIED ? "the codec policy refuses this codec" : "not in this build");
            return;
        }
        r = open_dec(k ? &A : &V, idx, k == 0, hwonly);
        if (r) { close_dec(&V); close_dec(&A); reply_error(r, tag, "the decoder would not open"); return; }
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if ((int)i != vi && (int)i != ai) fmt->streams[i]->discard = AVDISCARD_ALL;
    op.video = V.c ? V.idx : -1;
    op.audio = A.c ? A.idx : -1;
    if (V.c) {
        /* The pool is sized from the stream's parameters, before any frame is
         * decoded, so that every frame, the first included, can be decoded
         * into it: some decoders (MPEG-1/2/4) refuse a stride that changes
         * mid-stream. An unknown format is sized as 16-bit 4:4:4, the largest. */
        const AVCodecParameters *cp = fmt->streams[V.idx]->codecpar;
        enum AVPixelFormat sf = cp->format >= 0 ? (enum AVPixelFormat)cp->format : AV_PIX_FMT_YUV444P16LE;
        if (sf == AV_PIX_FMT_PAL8 || av_pix_fmt_desc_get(sf) == NULL) sf = AV_PIX_FMT_YUV444P16LE;
        int w = FFALIGN(cp->width > 0 ? cp->width : 64, 128) + 128, h = FFALIGN(cp->height > 0 ? cp->height : 64, 64) + 64;
        uint32_t off[4], pitch[4], rb[4], rows[4], total = 0;
        if (cp->width > 16384 || cp->height > 16384 || layout(sf, w, h, off, pitch, rb, rows, &total)) {
            close_dec(&V); close_dec(&A);
            reply_error(ACM_E_TOOBIG, 0, "picture size");
            return;
        }
        const AVPixFmtDescriptor *sd = av_pix_fmt_desc_get(sf);
        if (sf != AV_PIX_FMT_YUV444P16LE && sd && sd->comp[0].depth > 8) {   /* room for P010, a 10-bit read back */
            uint32_t t2;
            if (!layout(AV_PIX_FMT_YUV420P16LE, w, h, off, pitch, rb, rows, &t2) && t2 > total) total = t2;
        }
        op.width = (uint32_t)(cp->width > 0 ? cp->width : 0);
        op.height = (uint32_t)(cp->height > 0 ? cp->height : 0);
        op.pixfmt = acm_pix(cp->format >= 0 ? (enum AVPixelFormat)cp->format : AV_PIX_FMT_NONE);
        op.slot_size = (uint32_t)FFALIGN(total, 4096);
        uint32_t want_slots = 24 + 8;      /* also after a fall from hardware to software */
        uint32_t cap = (uint32_t)((768ull << 20) / op.slot_size);
        op.nslots = want_slots < cap ? want_slots : cap;
        if (op.nslots > 256) op.nslots = 256;
        if (op.nslots < 4) { close_dec(&V); close_dec(&A); reply_error(ACM_E_TOOBIG, 0, "frames too big for the pool"); return; }
        op.vpath = V.path;
    }
    if (A.c) {
        a_rate = (uint32_t)A.c->sample_rate;
        a_ch = A.c->ch_layout.nb_channels >= 2 ? 2 : 1;
        AVChannelLayout out = a_ch == 2 ? (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO : (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, (int)a_rate, &A.c->ch_layout, A.c->sample_fmt,
                                A.c->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
            reply_error(ACM_E_NOCODEC, 0, "audio conversion");
            return;
        }
        op.apath = A.path;
        op.sample_rate = a_rate;
        op.channels = a_ch;
    }
    acm_send(sock, ACM_A_OPENED, &op, sizeof op, NULL, 0, NULL, 0);
}

static void do_pool(const struct acm_pool *p, int fd)
{
    if (pool || fd < 0 || !p->slot_size || !p->nslots || p->nslots > 256 ||
        (uint64_t)p->slot_size * p->nslots != p->size) {
        if (fd >= 0) close(fd);
        reply_error(ACM_E_BADARG, 0, "pool");
        return;
    }
    struct stat st;
    if (fstat(fd, &st) || (uint64_t)st.st_size < p->size) { close(fd); reply_error(ACM_E_BADARG, 0, "pool size"); return; }
    void *m = mmap(NULL, p->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { reply_error(ACM_E_LIMIT, 0, "pool map"); return; }
    pool = m;
    slot_size = p->slot_size;
    nslots = p->nslots;
    reply_ok(NULL, 0);
}

/* ---- NEXT ---- */

static void fill_planes(struct acm_frame *fr, enum AVPixelFormat f, int w, int h)
{
    uint32_t total;
    layout(f, w, h, fr->offset, fr->pitch, fr->rowbytes, fr->rows, &total);
    fr->nplanes = 0;
    for (int i = 0; i < 4; i++) if (fr->pitch[i]) fr->nplanes = (uint32_t)i + 1;
}

/* Copies a software frame into a free slot. */
static int copy_to_slot(const AVFrame *f, struct acm_frame *fr)
{
    uint32_t total;
    if (layout(f->format, f->width, f->height, fr->offset, fr->pitch, fr->rowbytes, fr->rows, &total) || total > slot_size)
        return ACM_E_TOOBIG;
    int s = take_slot();
    if (s < 0) return ACM_E_AGAIN;
    uint8_t *base = pool + (size_t)s * slot_size;
    for (int i = 0; i < 4; i++) {
        if (!fr->pitch[i] || !f->data[i]) continue;
        if (fr->rows[i] == 1) memcpy(base + fr->offset[i], f->data[i], fr->rowbytes[i]);   /* a palette */
        else av_image_copy_plane(base + fr->offset[i], (int)fr->pitch[i], f->data[i], f->linesize[i],
                                 (int)fr->rowbytes[i], (int)fr->rows[i]);
    }
    fr->slot = (uint32_t)s;
    slot_given[s] = 1;
    fr->flags |= ACM_FF_COPIED;
    fr->nplanes = 0;
    for (int i = 0; i < 4; i++) if (fr->pitch[i]) fr->nplanes = (uint32_t)i + 1;
    return 0;
}

static int send_dmabuf(AVFrame *f, struct acm_frame *fr)
{
    AVFrame *m = f;
    if (f->format != AV_PIX_FMT_DRM_PRIME) {                     /* VAAPI: map to DRM PRIME */
        m = av_frame_alloc();
        if (!m) return ACM_E_LIMIT;
        m->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(m, f, AV_HWFRAME_MAP_READ) < 0) { av_frame_free(&m); return ACM_E_NOTAVAIL; }
    } else {
        m = av_frame_clone(f);
        if (!m) return ACM_E_LIMIT;
    }
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)m->data[0];
    int k = -1;
    for (int i = 0; i < HW_HELD; i++) if (!hw_held[i]) { k = i; break; }
    if (k < 0 || d->nb_objects > 4) { av_frame_free(&m); return ACM_E_AGAIN; }
    hw_held[k] = m;
    fr->slot = HW_HANDLE | (uint32_t)k;
    fr->flags |= ACM_FF_DMABUF;
    fr->nobj = (uint32_t)d->nb_objects;
    int fds[4];
    for (int i = 0; i < d->nb_objects; i++) { fds[i] = d->objects[i].fd; fr->objsize[i] = (uint32_t)d->objects[i].size; }
    fr->drm_modifier = d->objects[0].format_modifier;
    fr->drm_fourcc = d->nb_layers == 1 ? d->layers[0].format : 0;
    unsigned np = 0;
    for (int l = 0; l < d->nb_layers; l++)
        for (int p = 0; p < d->layers[l].nb_planes && np < 4; p++, np++) {
            fr->obj[np] = (uint32_t)d->layers[l].planes[p].object_index;
            fr->offset[np] = (uint32_t)d->layers[l].planes[p].offset;
            fr->pitch[np] = (uint32_t)d->layers[l].planes[p].pitch;
        }
    fr->nplanes = np;
    return acm_send(sock, ACM_A_FRAME, fr, sizeof *fr, NULL, 0, fds, d->nb_objects) ? ACM_E_IO : 1;
}

static void do_next_video(uint32_t want)
{
    if (!V.c) { reply_error(ACM_E_BADARG, 0, "no video open"); return; }
    if (!pool && !(want & ACM_WANT_DMABUF)) { reply_error(ACM_E_BADARG, 0, "no pool"); return; }
    AVFrame *f = av_frame_alloc();
    if (!f) { reply_error(ACM_E_LIMIT, 0, NULL); return; }
    int r = next_video(f);
    if (r) {
        av_frame_free(&f);
        char t[AV_ERROR_MAX_STRING_SIZE] = "";
        if (last_ff) av_strerror(last_ff, t, sizeof t);
        last_ff = 0;
        reply_error(r, 0, t);
        return;
    }
#ifdef ACM_TEST_HOOKS
    if (hook == ACM_HOOK_CRASH && frames_out >= hook_arg) *(volatile int *)(uintptr_t)8 = 1;
    if (hook == ACM_HOOK_HANG && frames_out >= hook_arg) for (;;) { }
    if (hook == ACM_HOOK_LEAK && frames_out >= hook_arg)
        for (;;) { void *x = malloc(64 << 20); if (!x) abort(); memset(x, 1, 64 << 20); }
#endif
    struct acm_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.path = V.path;
    if (V.path_changed) { fr.flags |= ACM_FF_PATH; V.path_changed = 0; }
    if (f->flags & AV_FRAME_FLAG_KEY) fr.flags |= ACM_FF_KEY;
    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    V.last_pts = pts;
    fr.pts_ms = pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(pts, fmt->streams[V.idx]->time_base, (AVRational){ 1, 1000 });
    fr.width = (uint32_t)f->width;
    fr.height = (uint32_t)f->height;
    frames_out++;
    if (f->hw_frames_ctx) {
        fr.flags |= ACM_FF_HW;
        AVHWFramesContext *hc = (AVHWFramesContext *)f->hw_frames_ctx->data;
        fr.pixfmt = acm_pix(hc->sw_format);
        if (want & ACM_WANT_DMABUF) {
            r = send_dmabuf(f, &fr);
            av_frame_free(&f);
            if (r != 1) reply_error(r, 0, "dma-buf");
            return;
        }
        AVFrame *sw = av_frame_alloc();
        if (!sw || av_hwframe_transfer_data(sw, f, 0) < 0) {
            av_frame_free(&sw); av_frame_free(&f);
            reply_error(ACM_E_NOTAVAIL, 0, "this hardware frame cannot be read back");
            return;
        }
        av_frame_free(&f);
        f = sw;
        fr.pixfmt = acm_pix(f->format);
        r = copy_to_slot(f, &fr);
        av_frame_free(&f);
        if (r) { reply_error(r, 0, "pool"); return; }
        acm_send(sock, ACM_A_FRAME, &fr, sizeof fr, NULL, 0, NULL, 0);
        return;
    }
    fr.pixfmt = acm_pix(f->format);
    /* decoded into the pool already? */
    int s = -1;
    if (pool && f->buf[0] && f->data[0] >= pool && f->data[0] < pool + (size_t)slot_size * nslots)
        s = (int)((f->data[0] - pool) / slot_size);
    if (s >= 0 && !slot_frame[s] && !f->buf[1]) {
        uint8_t *base = pool + (size_t)s * slot_size;
        fill_planes(&fr, f->format, f->width, f->height);
        for (int i = 0; i < 4; i++) {
            if (!fr.pitch[i]) continue;
            fr.offset[i] = (uint32_t)(f->data[i] - base);
            fr.pitch[i] = (uint32_t)f->linesize[i];
        }
        fr.slot = (uint32_t)s;
        slot_frame[s] = f;                                      /* held until the broker releases it */
        slot_given[s] = 1;
        acm_send(sock, ACM_A_FRAME, &fr, sizeof fr, NULL, 0, NULL, 0);
        return;
    }
    r = copy_to_slot(f, &fr);
    av_frame_free(&f);
    if (r) { reply_error(r, 0, "pool"); return; }
    acm_send(sock, ACM_A_FRAME, &fr, sizeof fr, NULL, 0, NULL, 0);
}

static void do_next_audio(void)
{
    if (!A.c) { reply_error(ACM_E_BADARG, 0, "no audio open"); return; }
    AVFrame *f;
    if (aqn) { f = aq[0]; memmove(aq, aq + 1, sizeof(aq[0]) * (size_t)--aqn); }
    else {
        f = av_frame_alloc();
        int r = f ? receive(&A, f) : ACM_E_LIMIT;
        if (r) { av_frame_free(&f); reply_error(r, 0, NULL); return; }
    }
    static int16_t buf[(ACM_MSG_MAX - 256) / 2];
    uint32_t maxf = (uint32_t)(sizeof buf / sizeof buf[0]) / a_ch;
    uint8_t *o[1] = { (uint8_t *)buf };
    int n = swr_convert(swr, o, (int)maxf, (const uint8_t **)f->extended_data, f->nb_samples);
    struct acm_audio au = { 0 };
    int64_t pts = f->best_effort_timestamp;
    au.pts_ms = pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(pts, fmt->streams[A.idx]->time_base, (AVRational){ 1, 1000 });
    av_frame_free(&f);
    if (n < 0) { reply_error(ACM_E_DECODER, 0, "audio conversion"); return; }
    au.frames = (uint32_t)n;
    au.sample_rate = a_rate;
    au.channels = a_ch;
    acm_send(sock, ACM_A_AUDIO, &au, sizeof au, buf, (uint32_t)n * a_ch * 2, NULL, 0);
}

static void do_release(uint32_t slot)
{
    if (slot & HW_HANDLE) {
        uint32_t k = slot & ~HW_HANDLE;
        if (k >= HW_HELD || !hw_held[k]) { reply_error(ACM_E_BADARG, slot, "no such frame"); return; }
        av_frame_free(&hw_held[k]);
        reply_ok(NULL, 0);
        return;
    }
    if (slot >= nslots || !slot_given[slot]) { reply_error(ACM_E_BADARG, slot, "slot not given out"); return; }
    slot_given[slot] = 0;
    if (slot_frame[slot]) av_frame_free(&slot_frame[slot]);     /* the decoder may still hold it: free_slot decides */
    else {
        pthread_mutex_lock(&slot_mx);
        slot_busy[slot] = 0;
        pthread_mutex_unlock(&slot_mx);
    }
    reply_ok(NULL, 0);
}

static void do_seek(const struct acm_seek *s)
{
    if (!fmt || (!V.c && !A.c)) { reply_error(ACM_E_BADARG, 0, "nothing open"); return; }
    if (src_size < 0) { reply_error(ACM_E_NOTAVAIL, 0, "not seekable"); return; }
    int64_t ts = s->ms * 1000;
    if (av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) { reply_error(ACM_E_EOF, 0, "seek"); return; }
    av_frame_free(&pending);
    for (int i = 0; i < aqn; i++) av_frame_free(&aq[i]);
    aqn = 0;
    if (V.c) { avcodec_flush_buffers(V.c); V.drained = 0; V.skip_until = AV_NOPTS_VALUE; }
    if (A.c) { avcodec_flush_buffers(A.c); A.drained = 0; }
    if (V.c && (s->flags & 1))                                    /* exact: drop frames before the time */
        V.skip_until = av_rescale_q(s->ms, (AVRational){ 1, 1000 }, fmt->streams[V.idx]->time_base) - 1;
    reply_ok(NULL, 0);
}

/* ---- test hooks ---- */

#ifdef ACM_TEST_HOOKS
#include <dirent.h>
#include <sys/socket.h>
#include <sys/wait.h>
static void do_hook(const struct acm_hook *h)
{
    if (h->hook != ACM_HOOK_ESCAPE) { hook = h->hook; hook_arg = h->arg; reply_ok(NULL, 0); return; }
    uint32_t worked = 0;
    if (h->arg == 0) {
        int fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { worked |= ACM_ESC_OPEN_PASSWD; close(fd); }
        fd = open("/tmp/acm-escape-test", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) { worked |= ACM_ESC_WRITE_TMP; close(fd); }
        DIR *d = opendir("/home");
        if (d) { worked |= ACM_ESC_LIST_HOME; closedir(d); }
        struct acm_error e = { 0, worked, "escape: file tests" };
        acm_send(sock, ACM_A_ERROR, &e, sizeof e, NULL, 0, NULL, 0);
        return;
    }
    /* each of these must kill the worker */
    if (h->arg == 1) { int s = socket(AF_INET, SOCK_STREAM, 0); (void)s; }
    if (h->arg == 2) { char *a[] = { "/bin/true", NULL }; char *e[] = { NULL }; execve(a[0], a, e); }
    if (h->arg == 3) { pid_t p = fork(); if (p == 0) _exit(0); }
    if (h->arg == 4) { void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); (void)m; }
    if (h->arg == 5) { kill(getppid(), SIGCONT); }
    struct acm_error e = { 0, 0xffffffffu, "escape: still alive" };
    acm_send(sock, ACM_A_ERROR, &e, sizeof e, NULL, 0, NULL, 0);
}
#endif

/* ---- the loop ---- */

static void open_hw_devices(void)
{
    av_log_set_level(AV_LOG_QUIET);
#if defined(__x86_64__)
    if (av_hwdevice_ctx_create(&hw_vaapi, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0) == 0)
        hw_paths |= ACM_PATH_HW_VAAPI;
#elif defined(__aarch64__)
    if (av_hwdevice_ctx_create(&hw_drm, AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0) == 0) {
        if (acm_caps_has_v4l2(1)) hw_paths |= ACM_PATH_HW_V4L2REQ;     /* a stateless HEVC decoder */
        if (acm_caps_has_v4l2(0)) hw_paths |= ACM_PATH_HW_V4L2M2M;     /* a stateful H.264 decoder */
    }
#endif
}

int main(int argc, char **argv)
{
    int hw = 0, trap = 0;
    unsigned mem = 512;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fd") && i + 1 < argc) sock = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hw")) hw = 1;
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) mem = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trap")) trap = 1;
        else { fprintf(stderr, "usage: acmedia-worker [--fd N] [--hw] [--mem MB] [--trap]\n"); return 2; }
    }
    if (setpriority(PRIO_PROCESS, 0, 5)) { /* a busy host: stay behind the Amiga */ }
    av_log_set_level(AV_LOG_QUIET);
    if (hw) open_hw_devices();
    struct acm_ready rd = { ACM_PROTO_VERSION, 0, hw_paths, 0 };
    if (acm_sandbox(hw, mem, trap, &rd.sandbox, &rd.landlock_abi)) {
        fprintf(stderr, "acmedia-worker: the sandbox would not go on (%#x); refusing to run\n", rd.sandbox);
        return 3;
    }
    for (;;) {
        uint32_t type, len;
        int fds[ACM_MAX_FDS], nfds = 0;
        int r = acm_recv(sock, &type, rx, sizeof rx, &len, fds, &nfds);
        if (r == 0) return 0;
        if (r < 0) { reply_error(ACM_E_BADARG, 0, "bad message"); continue; }
        int fd = nfds ? fds[0] : -1;
        for (int i = 1; i < nfds; i++) close(fds[i]);
#define NEED(T) if (len != sizeof(T)) { if (fd >= 0) close(fd); reply_error(ACM_E_BADARG, type, "size"); continue; }
        if (!hello_seen && type != ACM_Q_HELLO) { if (fd >= 0) close(fd); reply_error(ACM_E_BADARG, type, "HELLO first"); continue; }
        switch (type) {
        case ACM_Q_HELLO:
            NEED(struct acm_hello);
            if (hello_seen) { reply_error(ACM_E_BADARG, 0, "HELLO twice"); break; }
            memcpy(&hello, rx, sizeof hello);
            if (hello.version != ACM_PROTO_VERSION) { reply_error(ACM_E_VERSION, ACM_PROTO_VERSION, "protocol version"); break; }
            if (hello.nallow > ACM_MAX_TAGS) hello.nallow = ACM_MAX_TAGS;
            if (hello.nhwonly > ACM_MAX_TAGS) hello.nhwonly = ACM_MAX_TAGS;
            copy_only = !!(hello.flags & ACM_HF_COPY);
            hello_seen = 1;
            reply_ok(&rd, sizeof rd);
            break;
        case ACM_Q_SOURCE: {
            NEED(struct acm_source);
            if (fd < 0 || src_fd >= 0) { if (fd >= 0) close(fd); reply_error(ACM_E_BADARG, 0, "source"); break; }
            int fl = fcntl(fd, F_GETFL);
            if (fl < 0 || (fl & O_ACCMODE) != O_RDONLY) { close(fd); reply_error(ACM_E_BADARG, 0, "source must be read only"); break; }
            src_fd = fd;
            struct stat st;
            src_size = (!fstat(fd, &st) && S_ISREG(st.st_mode)) ? (int64_t)st.st_size : -1;
            reply_ok(NULL, 0);
            break;
        }
        case ACM_Q_PROBE: do_probe(); break;
        case ACM_Q_OPEN: NEED(struct acm_open); do_open((const struct acm_open *)rx); break;
        case ACM_Q_POOL: NEED(struct acm_pool); do_pool((const struct acm_pool *)rx, fd); fd = -1; break;
        case ACM_Q_NEXT: {
            NEED(struct acm_next);
            const struct acm_next *n = (const void *)rx;
            if (n->want & ACM_WANT_AUDIO) do_next_audio(); else do_next_video(n->want);
            break;
        }
        case ACM_Q_RELEASE: NEED(struct acm_release); do_release(((const struct acm_release *)rx)->slot); break;
        case ACM_Q_SEEK: NEED(struct acm_seek); do_seek((const struct acm_seek *)rx); break;
        case ACM_Q_CAPS: {
            struct acm_caps c;
            acm_caps_fill(&c, hw_paths, hw_vaapi);
            acm_send(sock, ACM_A_CAPS, &c, sizeof c, NULL, 0, NULL, 0);
            break;
        }
#ifdef ACM_TEST_HOOKS
        case ACM_Q_HOOK: NEED(struct acm_hook); do_hook((const struct acm_hook *)rx); break;
#endif
        case ACM_Q_CLOSE: reply_ok(NULL, 0); return 0;
        default: reply_error(ACM_E_BADARG, type, "unknown request"); break;
        }
        if (fd >= 0 && type != ACM_Q_SOURCE) close(fd);
    }
}
