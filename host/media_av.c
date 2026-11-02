/*
 * media.decode/1 on the host, sound and video (docs/MEDIA_DECODE.md): any
 * audio FFmpeg reads (FLAC, Ogg Vorbis, Opus, MP3, AAC, ALAC, WMA, WAV...)
 * answered as 16-bit big-endian PCM at the rate and channels asked for, and
 * any video (H.264, HEVC, AV1, VP9, MPEG-4, WMV...) kept open and answered
 * frame by frame in 256 colours or 24-bit, scaled to fit.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "media_decode.h"
#include "media_av.h"
#include "ham8.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>

/* Longer sounds are refused: 2^28 frames is 1 GB of 16-bit stereo. */
#define MAX_FRAMES (1u << 28)

struct mem {
    const uint8_t *data;
    int64_t size, pos;
};

static int mem_read(void *opaque, uint8_t *buf, int n)
{
    struct mem *m = opaque;
    int64_t left = m->size - m->pos;
    if (left <= 0)
        return AVERROR_EOF;
    if (n > left)
        n = (int)left;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence)
{
    struct mem *m = opaque;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return m->size;
    case SEEK_SET: break;
    case SEEK_CUR: off += m->pos; break;
    case SEEK_END: off += m->size; break;
    default: return -1;
    }
    if (off < 0 || off > m->size)
        return -1;
    return m->pos = off;
}

struct sound {
    struct mem mem;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream;
    uint32_t format, rate, channels;     /* the answer's */
};

static void sound_close(struct sound *s)
{
    av_frame_free(&s->frame);
    av_packet_free(&s->pkt);
    swr_free(&s->swr);
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
    if (s->io) {
        av_freep(&s->io->buffer);
        avio_context_free(&s->io);
    }
}

static uint32_t fourcc(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_FLAC: return MD_FORMAT_FLAC;
    case AV_CODEC_ID_VORBIS: return MD_FORMAT_VORBIS;
    case AV_CODEC_ID_OPUS: return MD_FORMAT_OPUS;
    case AV_CODEC_ID_MP3: return MD_FORMAT_MP3;
    case AV_CODEC_ID_AAC: return MD_FORMAT_AAC;
    case AV_CODEC_ID_ALAC: return MD_FORMAT_ALAC;
    case AV_CODEC_ID_WMAV1: case AV_CODEC_ID_WMAV2: case AV_CODEC_ID_WMAPRO: case AV_CODEC_ID_WMALOSSLESS:
        return MD_FORMAT_WMA;
    default: return MD_FORMAT_SOUND;
    }
}

/* Opens the best audio stream; the answer has at most maxch channels and a
 * rate of at most maxrate (0: the file's), halving the file's rate until it fits. */
static int sound_open(struct sound *s, const uint8_t *d, uint32_t n, uint32_t maxch, uint32_t maxrate)
{
    const AVCodec *codec = NULL;
    AVChannelLayout out_layout;
    uint8_t *iobuf;
    int st;

    memset(s, 0, sizeof *s);
    av_log_set_level(AV_LOG_ERROR);
    s->mem.data = d;
    s->mem.size = n;
    if (!(iobuf = av_malloc(65536)))
        return MD_HOSTERROR;
    if (!(s->io = avio_alloc_context(iobuf, 65536, 0, &s->mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return MD_HOSTERROR;
    }
    if (!(s->fmt = avformat_alloc_context()))
        return MD_HOSTERROR;
    s->fmt->pb = s->io;
    if (avformat_open_input(&s->fmt, NULL, NULL, NULL) < 0)
        return MD_BADREQUEST;                 /* frees fmt */
    if (avformat_find_stream_info(s->fmt, NULL) < 0)
        return MD_BADREQUEST;
    if ((s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) < 0 || !codec)
        return MD_BADREQUEST;
    if (!(s->dec = avcodec_alloc_context3(codec)))
        return MD_HOSTERROR;
    if (avcodec_parameters_to_context(s->dec, s->fmt->streams[s->stream]->codecpar) < 0 ||
        avcodec_open2(s->dec, codec, NULL) < 0 || s->dec->sample_rate <= 0 || s->dec->ch_layout.nb_channels <= 0)
        return MD_BADREQUEST;

    s->format = !strcmp(s->fmt->iformat->name, "libopenmpt") ? MD_FORMAT_MODULE
              : !strcmp(s->fmt->iformat->name, "libgme") ? MD_FORMAT_CHIPTUNE : fourcc(codec->id);
    s->channels = s->dec->ch_layout.nb_channels;
    if (maxch && s->channels > maxch)
        s->channels = maxch;
    if (s->channels > 2)
        s->channels = 2;
    s->rate = s->dec->sample_rate;
    while (maxrate && s->rate > maxrate && s->rate >= 2000)
        s->rate /= 2;
    if (maxrate && s->rate > maxrate)
        s->rate = maxrate;

    av_channel_layout_default(&out_layout, s->channels);
    st = swr_alloc_set_opts2(&s->swr, &out_layout, AV_SAMPLE_FMT_S16, s->rate,
                             &s->dec->ch_layout, s->dec->sample_fmt, s->dec->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (st < 0 || swr_init(s->swr) < 0)
        return MD_HOSTERROR;
    if (!(s->pkt = av_packet_alloc()) || !(s->frame = av_frame_alloc()))
        return MD_HOSTERROR;
    return MD_OK;
}

/* Where decoded frames go: frames before skip are dropped; the rest are
 * written to out (big-endian) until room frames are written, and counted. */
struct sink {
    uint64_t skip, room, pos, written;
    uint8_t *out;
    uint32_t channels;
};

static void sink_put(struct sink *k, const int16_t *pcm, int n)
{
    int i;
    uint32_t c;

    for (i = 0; i < n; i++, k->pos++) {
        if (k->pos < k->skip || !k->out || k->written >= k->room)
            continue;
        for (c = 0; c < k->channels; c++) {
            int16_t v = pcm[i * k->channels + c];
            uint8_t *o = k->out + (k->written * k->channels + c) * 2;
            o[0] = (uint8_t)((uint16_t)v >> 8);
            o[1] = (uint8_t)v;
        }
        k->written++;
    }
}

/* Runs in (NULL: flushes) through the resampler into the sink. */
static int resample(struct sound *s, struct sink *k, const uint8_t **in, int n)
{
    int room = swr_get_out_samples(s->swr, n), got;
    int16_t *pcm;

    if (room <= 0)
        return 0;
    if (!(pcm = av_malloc((size_t)room * s->channels * 2)))
        return -1;
    got = swr_convert(s->swr, (uint8_t **)&pcm, room, in, n);
    if (got > 0)
        sink_put(k, pcm, got);
    av_free(pcm);
    return got < 0 ? -1 : 0;
}

static void drain(struct sound *s, struct sink *k)
{
    while (avcodec_receive_frame(s->dec, s->frame) == 0) {
        resample(s, k, (const uint8_t **)s->frame->extended_data, s->frame->nb_samples);
        av_frame_unref(s->frame);
    }
}

/* Decodes the whole stream through the resampler into k. */
static void sound_run(struct sound *s, struct sink *k)
{
    k->channels = s->channels;
    while (av_read_frame(s->fmt, s->pkt) >= 0 && k->pos < MAX_FRAMES + k->skip + 1) {
        if (s->pkt->stream_index == s->stream && avcodec_send_packet(s->dec, s->pkt) >= 0)
            drain(s, k);
        av_packet_unref(s->pkt);
    }
    avcodec_send_packet(s->dec, NULL);
    drain(s, k);
    resample(s, k, NULL, 0);
}

int md_sound_probe(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                   uint32_t *result, uint32_t *aux)
{
    struct sound s;
    struct sink k = { 0 };
    uint64_t frames;
    int st = sound_open(&s, file->in, file->length, extra[0], extra[1]);

    if (st == MD_OK) {
        sound_run(&s, &k);
        frames = k.pos;
        if (frames > MAX_FRAMES)
            st = MD_BADREQUEST;
        else {
            md_put32(info, MD_KIND_SOUND);
            md_put32(info + 4, s.format);
            md_put32(info + 8, 0);
            md_put32(info + 12, (uint32_t)frames);
            md_put32(info + 16, s.rate);
            md_put32(info + 20, s.channels);
            *result = (uint32_t)frames;
            *aux = s.rate;
        }
    }
    sound_close(&s);
    return st;
}

int md_sound_decode(const struct md_buffer *file, uint32_t first, const uint32_t extra[4], struct md_buffer *out,
                    uint32_t *result, uint32_t *aux)
{
    struct sound s;
    struct sink k = { 0 };
    int st = sound_open(&s, file->in, file->length, extra[0], extra[1]);

    if (st == MD_OK) {
        if (out->length < s.channels * 2)
            st = MD_TOOSMALL;
        else {
            k.skip = first;
            k.out = out->out;
            k.room = out->length / (s.channels * 2);
            sound_run(&s, &k);
            out->written = (uint32_t)(k.written * s.channels * 2);
            *result = (uint32_t)k.written;
            *aux = s.rate;
        }
    }
    sound_close(&s);
    return st;
}

/* 1 when FFmpeg recognises the file's first bytes. */
static const AVInputFormat *probe(const uint8_t *d, uint32_t n)
{
    const AVInputFormat *f;
    AVProbeData pd = { 0 };
    uint8_t *buf;
    int score = 0;

    av_log_set_level(AV_LOG_ERROR);
    n = n > 65536 ? 65536 : n;
    if (!(buf = av_mallocz(n + AVPROBE_PADDING_SIZE)))
        return NULL;
    memcpy(buf, d, n);
    pd.filename = "";
    pd.buf = buf;
    pd.buf_size = n;
    f = av_probe_input_format3(&pd, 1, &score);
    av_free(buf);
    return score >= AVPROBE_SCORE_MAX / 4 ? f : NULL;
}

/* A still picture: one of FFmpeg's piped image readers (jpegxl_pipe, ...). */
/* The entry of an icon file's directory with the most pixels (a width or
 * height of 0 there means 256); -1 when the directory is short. */
static int ico_largest(const uint8_t *d, uint32_t n)
{
    uint32_t count, i, best = 0;
    int pick = -1;

    if (n < 6)
        return -1;
    count = d[4] | d[5] << 8;
    for (i = 0; i < count && 6 + 16 * (i + 1) <= n; i++) {
        const uint8_t *e = d + 6 + 16 * i;
        uint32_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        if (w * h > best) {
            best = w * h;
            pick = (int)i;
        }
    }
    return pick;
}

static int is_still(const AVInputFormat *f)
{
    size_t len = strlen(f->name);
    /* GIF and APNG answer PROBE and DECODE with their first frame, an icon
     * file with its first icon. */
    return (len > 5 && !strcmp(f->name + len - 5, "_pipe")) || !strcmp(f->name, "gif") || !strcmp(f->name, "apng")
           || !strcmp(f->name, "ico");
}

int md_is_av(const uint8_t *d, uint32_t n)
{
    const AVInputFormat *f = probe(d, n);
    return !f ? MD_AV_NONE : is_still(f) ? MD_AV_STILL : MD_AV_MEDIA;
}

/* ---- still pictures ------------------------------------------------------ */

static uint32_t still_fourcc(const char *name)
{
    static const struct { const char *name; uint32_t fourcc; } map[] = {
        { "jpeg_pipe", MD_FORMAT_JPEG }, { "png_pipe", MD_FORMAT_PNG }, { "gif_pipe", MD_FORMAT_GIF }, { "gif", MD_FORMAT_GIF },
        { "apng", MD_FORMAT_PNG }, { "ico", MD_FORMAT_ICO },
        { "jpegxl_pipe", MD_FORMAT_JXL }, { "exr_pipe", MD_FORMAT_EXR }, { "hdr_pipe", MD_FORMAT_HDR },
        { "psd_pipe", MD_FORMAT_PSD }, { "qoi_pipe", MD_FORMAT_QOI }, { "dds_pipe", MD_FORMAT_DDS },
        { "j2k_pipe", MD_FORMAT_J2K }, { "tiff_pipe", MD_FORMAT_TIFF }, { "webp_pipe", MD_FORMAT_WEBP },
        { "dpx_pipe", MD_FORMAT_DPX }, { "pcx_pipe", MD_FORMAT_PCX }, { "sgi_pipe", MD_FORMAT_SGI },
    };
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(name, map[i].name))
            return map[i].fourcc;
    return MD_FORMAT_STILL;
}

int md_still_load(const uint8_t *d, uint32_t n, int decode, uint32_t *format, uint32_t *flags,
                  uint32_t *width, uint32_t *height, uint8_t **argb)
{
    const AVInputFormat *f = probe(d, n);
    struct mem mem = { d, n, 0 };
    AVIOContext *io = NULL;
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    struct SwsContext *sws = NULL;
    uint8_t *iobuf;
    int st = MD_BADREQUEST, stream, got = 0;

    *argb = NULL;
    if (!f || !is_still(f))
        return MD_BADREQUEST;
    if (!(iobuf = av_malloc(65536)))
        return MD_HOSTERROR;
    if (!(io = avio_alloc_context(iobuf, 65536, 0, &mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return MD_HOSTERROR;
    }
    if (!(fmt = avformat_alloc_context()))
        goto out;
    fmt->pb = io;
    if (avformat_open_input(&fmt, NULL, f, NULL) < 0)
        goto out;
    /* No avformat_find_stream_info: it decodes the picture once just to look. */
    if ((stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0)
        goto out;
    /* An icon file holds the same icon at several sizes: the largest, by the
     * file's own directory (FFmpeg leaves a PNG icon's size at 0 until it is
     * decoded); its streams are the directory's entries, in order. */
    if (!strcmp(f->name, "ico")) {
        int big = ico_largest(d, n);
        if (big >= 0 && (unsigned)big < fmt->nb_streams) {
            const AVCodec *c = avcodec_find_decoder(fmt->streams[big]->codecpar->codec_id);
            if (c) {
                stream = big;
                codec = c;
            }
        }
    }
    if (!(dec = avcodec_alloc_context3(codec))
        || avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0
        || avcodec_open2(dec, codec, NULL) < 0)
        goto out;
    *format = still_fourcc(f->name);
    *flags = 0;
    *width = dec->width;
    *height = dec->height;
    /* An icon's alpha (a BMP icon's bit depth) shows only once it is decoded. */
    if (!decode && *width && *height && strcmp(f->name, "ico")) {
        const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(dec->pix_fmt);
        if (pd && (pd->flags & AV_PIX_FMT_FLAG_ALPHA))
            *flags = MD_FLAG_ALPHA;
        st = MD_OK;
        goto out;
    }
    if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc()))
        goto out;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt) >= 0)
            got = avcodec_receive_frame(dec, frame) >= 0;
        av_packet_unref(pkt);
    }
    if (!got && avcodec_send_packet(dec, NULL) >= 0)
        got = avcodec_receive_frame(dec, frame) >= 0;
    if (!got || frame->width <= 0 || frame->height <= 0
        || (uint64_t)frame->width * frame->height > (1u << 28))
        goto out;
    {
        const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(frame->format);
        int stride = frame->width * 4;
        *width = frame->width;
        *height = frame->height;
        *flags = pd && (pd->flags & AV_PIX_FMT_FLAG_ALPHA) ? MD_FLAG_ALPHA : 0;
        if (!decode) {
            st = MD_OK;
            goto out;
        }
        if (!(sws = sws_getContext(frame->width, frame->height, frame->format, frame->width, frame->height,
                                   AV_PIX_FMT_ARGB, SWS_POINT, NULL, NULL, NULL))
            || !(*argb = malloc((size_t)stride * frame->height))) {
            st = MD_HOSTERROR;
            goto out;
        }
        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, argb, &stride);
        if (!*flags) {
            size_t i, px = (size_t)frame->width * frame->height;
            for (i = 0; i < px; i++)
                (*argb)[i * 4] = 255;
        }
        st = MD_OK;
    }
out:
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    return st;
}

/* ---- video --------------------------------------------------------------- */

/* Open videos, by handle (index + 1). A video stays open, with its own copy
 * of the file, until VCLOSE or until MAX_VIDEOS newer ones push it out. */
#define MAX_VIDEOS 8

struct video {
    int used;
    uint32_t serial;           /* for pushing out the oldest */
    uint8_t *file;
    struct mem mem;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    struct SwsContext *sws;
    AVPacket *pkt;
    AVFrame *frame, *rgb;
    int stream;
    int64_t *pts;              /* each source frame's time stamp, in presentation order */
    uint32_t *map;             /* frame asked -> source frame, when frames last unevenly; else NULL */
    uint32_t frames, nsrc, ow, oh;   /* frames asked for (evenly timed), source frames */
    uint32_t shown;            /* the source frame in rgb; UINT32_MAX: none */
    int64_t next;              /* the source frame the decoder gives next; -1: unknown (seek) */
    uint32_t fps1000;          /* frames a second x 1000, as VOPEN answered */
    int64_t start_us;          /* time zero for frames and sound, in AV_TIME_BASE units */
    /* A stream opened from a URL (VOPENURL): read forward only, never
     * scanned or sought. Frames are asked for by the clock. */
    int live;
    int64_t shown_us;          /* live: the time of the frame in rgb */
    int eof;                   /* live: the stream has ended */
    AVPacket **vq;             /* live: video packets read ahead while sound was wanted */
    uint32_t vq_head, vq_n, vq_cap;
    struct track *sound;       /* VSOUND's, opened at the first VSOUND */
};

/* A video's sound for VSOUND: 8-bit signed mono at the rate asked, kept as
 * a window of samples by their place from time zero. A file's sound has a
 * demuxer of its own, over the same bytes, so it can seek apart from the
 * pictures; a live stream's comes from the one demuxer, in turn with them. */
struct track {
    struct mem mem;
    AVIOContext *io;
    AVFormatContext *fmt;      /* NULL for a live stream: the video's is used */
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream;
    uint32_t rate;
    int eof;
    int8_t *pcm;
    int64_t first;             /* the sample pcm[0] is, counted from time zero */
    size_t len, cap;
    int64_t next;              /* where the next decoded sample goes; INT64_MIN: from its time stamp */
};

static struct video videos[MAX_VIDEOS];
static uint32_t video_serial;
static pthread_mutex_t video_lock = PTHREAD_MUTEX_INITIALIZER;

static void track_free(struct track *t)
{
    if (!t)
        return;
    av_frame_free(&t->frame);
    av_packet_free(&t->pkt);
    swr_free(&t->swr);
    avcodec_free_context(&t->dec);
    avformat_close_input(&t->fmt);
    if (t->io) {
        av_freep(&t->io->buffer);
        avio_context_free(&t->io);
    }
    free(t->pcm);
    free(t);
}

static void video_free(struct video *v)
{
    while (v->vq_n) {
        av_packet_free(&v->vq[v->vq_head]);
        v->vq_head = (v->vq_head + 1) % v->vq_cap;
        v->vq_n--;
    }
    free(v->vq);
    track_free(v->sound);
    av_frame_free(&v->frame);
    av_frame_free(&v->rgb);
    av_packet_free(&v->pkt);
    sws_freeContext(v->sws);
    avcodec_free_context(&v->dec);
    avformat_close_input(&v->fmt);
    if (v->io) {
        av_freep(&v->io->buffer);
        avio_context_free(&v->io);
    }
    free(v->pts);
    free(v->map);
    free(v->file);
    memset(v, 0, sizeof *v);
}

static uint32_t video_fourcc(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return MD_FORMAT_H264;
    case AV_CODEC_ID_HEVC: return MD_FORMAT_HEVC;
    case AV_CODEC_ID_AV1: return MD_FORMAT_AV1;
    case AV_CODEC_ID_VP8: return MD_FORMAT_VP8;
    case AV_CODEC_ID_VP9: return MD_FORMAT_VP9;
    case AV_CODEC_ID_MPEG4: return MD_FORMAT_MPEG4;
    case AV_CODEC_ID_MPEG1VIDEO: case AV_CODEC_ID_MPEG2VIDEO: return MD_FORMAT_MPEG2;
    case AV_CODEC_ID_WMV1: case AV_CODEC_ID_WMV2: case AV_CODEC_ID_WMV3: case AV_CODEC_ID_VC1: return MD_FORMAT_WMV;
    case AV_CODEC_ID_MJPEG: return MD_FORMAT_MJPEG;
    case AV_CODEC_ID_THEORA: return MD_FORMAT_THEORA;
    default: return MD_FORMAT_VIDEO;
    }
}

static int cmp64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* Frames that last unevenly (a GIF or APNG pausing on one picture, or a
 * variable-rate video) are played at one steady rate: the shortest frame's,
 * at most 50 a second, each source frame repeated for as long as it lasts.
 * The Amiga then asks for frames by a steady clock, as for any video. */
static int even_out(struct video *v, AVRational tb, int64_t last_dur, uint32_t *fps1000)
{
    int64_t tick = 0, most = 0, floor_tick, span, i;
    uint32_t k, n, src = 0;

    if (v->nsrc < 2 || tb.num <= 0 || tb.den <= 0)
        return MD_OK;
    for (i = 1; i < v->nsrc; i++) {
        int64_t d = v->pts[i] - v->pts[i - 1];
        if (d > 0 && (!tick || d < tick))
            tick = d;
        if (d > most)
            most = d;
    }
    if (last_dur > most)
        most = last_dur;
    /* Steady enough already: time stamps that wobble by a tick or so. */
    if (!tick || most * 2 < tick * 3)
        return MD_OK;
    if (last_dur <= 0)
        last_dur = tick;
    floor_tick = av_rescale_q(1, (AVRational){ 1, 50 }, tb);
    if (tick < floor_tick)
        tick = floor_tick > 0 ? floor_tick : 1;
    span = v->pts[v->nsrc - 1] - v->pts[0] + last_dur;
    n = span / tick + (span % tick != 0);
    if (!n || n > 200000)
        return MD_OK;                  /* far too long: kept as it is */
    if (!(v->map = malloc((size_t)n * sizeof *v->map)))
        return MD_HOSTERROR;
    for (k = 0; k < n; k++) {
        int64_t t = v->pts[0] + (int64_t)k * tick;
        while (src + 1 < v->nsrc && v->pts[src + 1] <= t)
            src++;
        v->map[k] = src;
    }
    v->frames = n;
    *fps1000 = (uint32_t)((int64_t)tb.den * 1000 / ((int64_t)tb.num * tick));
    return MD_OK;
}

/* Opens a video from n bytes at d: the video's own copy, or d itself when
 * own (an upload, handed over). */
static int video_open(struct video *v, const uint8_t *d, uint32_t n, int own, uint32_t maxw, uint32_t maxh,
                      uint32_t *fps1000)
{
    const AVCodec *codec = NULL;
    AVStream *st;
    uint8_t *iobuf;
    uint32_t cap = 0;
    AVRational rate;
    int64_t last_t = 0, last_dur = 0;

    av_log_set_level(AV_LOG_ERROR);
    if (own)
        v->file = (uint8_t *)d;
    else {
        if (!(v->file = malloc(n)))
            return MD_HOSTERROR;
        memcpy(v->file, d, n);
    }
    v->mem.data = v->file;
    v->mem.size = n;
    if (!(iobuf = av_malloc(65536)))
        return MD_HOSTERROR;
    if (!(v->io = avio_alloc_context(iobuf, 65536, 0, &v->mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return MD_HOSTERROR;
    }
    if (!(v->fmt = avformat_alloc_context()))
        return MD_HOSTERROR;
    v->fmt->pb = v->io;
    if (avformat_open_input(&v->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(v->fmt, NULL) < 0)
        return MD_BADREQUEST;
    if ((v->stream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0 || !codec)
        return MD_BADREQUEST;
    st = v->fmt->streams[v->stream];
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        return MD_BADREQUEST;                 /* cover art in a sound file */
    if (!(v->dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(v->dec, st->codecpar) < 0)
        return MD_HOSTERROR;
    v->dec->thread_count = 4;
    if (avcodec_open2(v->dec, codec, NULL) < 0 || v->dec->width <= 0 || v->dec->height <= 0)
        return MD_BADREQUEST;
    if (!(v->pkt = av_packet_alloc()) || !(v->frame = av_frame_alloc()) || !(v->rgb = av_frame_alloc()))
        return MD_HOSTERROR;

    /* Every frame's time stamp, from the packets alone (no decoding). */
    while (av_read_frame(v->fmt, v->pkt) >= 0) {
        if (v->pkt->stream_index == v->stream) {
            int64_t t = v->pkt->pts != AV_NOPTS_VALUE ? v->pkt->pts : v->pkt->dts;
            if (t != AV_NOPTS_VALUE && (!v->frames || t >= last_t)) {
                last_t = t;
                last_dur = v->pkt->duration;
            }
            if (v->frames == cap) {
                int64_t *more = realloc(v->pts, (cap = cap ? cap * 2 : 1024) * sizeof *more);
                if (!more) {
                    av_packet_unref(v->pkt);
                    return MD_HOSTERROR;
                }
                v->pts = more;
            }
            v->pts[v->frames] = t == AV_NOPTS_VALUE ? (int64_t)v->frames : t;
            v->frames++;
        }
        av_packet_unref(v->pkt);
    }
    if (!v->frames)
        return MD_BADREQUEST;
    qsort(v->pts, v->frames, sizeof *v->pts, cmp64);
    v->next = -1;
    v->shown = UINT32_MAX;
    v->nsrc = v->frames;

    md_fit(v->dec->width, v->dec->height, maxw, maxh, &v->ow, &v->oh);
    if (!(v->sws = sws_getContext(v->dec->width, v->dec->height, v->dec->pix_fmt, v->ow, v->oh, AV_PIX_FMT_RGB24,
                                  SWS_AREA, NULL, NULL, NULL)))
        return MD_HOSTERROR;
    v->rgb->format = AV_PIX_FMT_RGB24;
    v->rgb->width = v->ow;
    v->rgb->height = v->oh;
    if (av_frame_get_buffer(v->rgb, 0) < 0)
        return MD_HOSTERROR;
    rate = av_guess_frame_rate(v->fmt, st, NULL);
    *fps1000 = rate.num > 0 && rate.den > 0 ? (uint32_t)((int64_t)rate.num * 1000 / rate.den) : 25000;
    /* Time zero for the sound is the first frame's. */
    v->start_us = av_rescale_q(v->pts[0], st->time_base, AV_TIME_BASE_Q);
    return even_out(v, st->time_base, last_dur, fps1000);
}

/* The 6x6x6 colour cube with 4x4 ordered dithering: index i < 216 is red
 * (i / 36), green (i / 6 % 6) and blue (i % 6), each step 51, as
 * gfopenimage's webm.datatype draws. */
static void dither(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, uint8_t *out)
{
    static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)y * pitch;
        for (x = 0; x < w; x++, s += 3) {
            int dd = bayer[y & 3][x & 3] * 16;
            *out++ = (uint8_t)(((s[0] * 5 + dd) >> 8) * 36 + ((s[1] * 5 + dd) >> 8) * 6 + ((s[2] * 5 + dd) >> 8));
        }
    }
}

/* Decodes up to frame index want (by its time stamp) and leaves it in v->rgb. */
static int video_seek_decode(struct video *v, uint32_t want)
{
    int64_t target = v->pts[want];
    int eof = 0;

    if (v->next < 0 || want < v->next || want > v->next + 50) {
        avcodec_flush_buffers(v->dec);
        if (av_seek_frame(v->fmt, v->stream, target, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(v->fmt, v->stream, v->pts[0], AVSEEK_FLAG_BACKWARD);
        v->next = -1;
    }
    for (;;) {
        int r = avcodec_receive_frame(v->dec, v->frame);
        if (r == 0) {
            int64_t t = v->frame->best_effort_timestamp;
            if (t == AV_NOPTS_VALUE || t >= target) {
                sws_scale(v->sws, (const uint8_t *const *)v->frame->data, v->frame->linesize, 0, v->dec->height,
                          v->rgb->data, v->rgb->linesize);
                av_frame_unref(v->frame);
                v->next = want + 1;
                return MD_OK;
            }
            av_frame_unref(v->frame);
            continue;
        }
        if (r == AVERROR_EOF || (r != AVERROR(EAGAIN)))
            return MD_BADREQUEST;
        if (eof)
            return MD_BADREQUEST;
        if (av_read_frame(v->fmt, v->pkt) < 0) {
            avcodec_send_packet(v->dec, NULL);
            eof = 1;
            continue;
        }
        if (v->pkt->stream_index == v->stream)
            avcodec_send_packet(v->dec, v->pkt);
        av_packet_unref(v->pkt);
    }
}

/* ---- live streams (VOPENURL) ------------------------------------------- */

/* The protocols a URL may use: the network only, never the host's files. */
#define LIVE_PROTOCOLS "http,https,tls,tcp,hls,crypto,httpproxy"
/* Video packets kept while reading ahead for sound: a few seconds' worth. */
#define LIVE_QUEUE 1500

static int vq_push(struct video *v, AVPacket *pkt)
{
    AVPacket *p;

    if (v->vq_n == v->vq_cap) {
        uint32_t cap = v->vq_cap ? v->vq_cap * 2 : 64, i;
        AVPacket **q = malloc(cap * sizeof *q);
        if (!q)
            return -1;
        for (i = 0; i < v->vq_n; i++)
            q[i] = v->vq[(v->vq_head + i) % v->vq_cap];
        free(v->vq);
        v->vq = q;
        v->vq_cap = cap;
        v->vq_head = 0;
    }
    if (!(p = av_packet_clone(pkt)))
        return -1;
    v->vq[(v->vq_head + v->vq_n) % v->vq_cap] = p;
    v->vq_n++;
    return 0;
}

static int64_t frame_us(const struct video *v, uint32_t index)
{
    return v->start_us + (int64_t)index * 1000000 * 1000 / (v->fps1000 ? v->fps1000 : 25000);
}

static void track_take(struct video *v, struct track *t, AVPacket *pkt);

/* Reads the live stream's next packet: a video packet is left in v->pkt
 * (1), a sound packet goes to the sound track (0); -1 at the end. */
static int live_read(struct video *v)
{
    for (;;) {
        if (v->eof || av_read_frame(v->fmt, v->pkt) < 0) {
            v->eof = 1;
            return -1;
        }
        if (v->pkt->stream_index == v->stream)
            return 1;
        if (v->sound && v->pkt->stream_index == v->sound->stream)
            track_take(v, v->sound, v->pkt);
        av_packet_unref(v->pkt);
    }
}

static int video_open_url(struct video *v, const char *url, uint32_t maxw, uint32_t maxh, uint32_t *fps1000)
{
    const AVCodec *codec = NULL;
    AVDictionary *opts = NULL;
    AVStream *st;
    AVRational rate;
    int r;

    av_log_set_level(AV_LOG_ERROR);
    av_dict_set(&opts, "protocol_whitelist", LIVE_PROTOCOLS, 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    r = avformat_open_input(&v->fmt, url, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0 || avformat_find_stream_info(v->fmt, NULL) < 0)
        return MD_BADREQUEST;
    if ((v->stream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0 || !codec)
        return MD_BADREQUEST;
    st = v->fmt->streams[v->stream];
    if (!(v->dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(v->dec, st->codecpar) < 0)
        return MD_HOSTERROR;
    v->dec->thread_count = 4;
    if (avcodec_open2(v->dec, codec, NULL) < 0 || v->dec->width <= 0 || v->dec->height <= 0)
        return MD_BADREQUEST;
    if (!(v->pkt = av_packet_alloc()) || !(v->frame = av_frame_alloc()) || !(v->rgb = av_frame_alloc()))
        return MD_HOSTERROR;
    /* The scaler is made at the first frame, and again whenever the
     * stream's pictures change size or format (an HLS stream moving to
     * another of its variants): the output size stays. */
    md_fit(v->dec->width, v->dec->height, maxw, maxh, &v->ow, &v->oh);
    v->rgb->format = AV_PIX_FMT_RGB24;
    v->rgb->width = v->ow;
    v->rgb->height = v->oh;
    if (av_frame_get_buffer(v->rgb, 0) < 0)
        return MD_HOSTERROR;
    rate = av_guess_frame_rate(v->fmt, st, NULL);
    *fps1000 = rate.num > 0 && rate.den > 0 ? (uint32_t)((int64_t)rate.num * 1000 / rate.den) : 25000;
    if (*fps1000 < 1000 || *fps1000 > 60000)
        *fps1000 = 25000;
    v->live = 1;
    v->shown = UINT32_MAX;
    /* Time zero is the first picture's: read on to it (sound before it is
     * dropped) and keep the packet for the decoder. */
    if (live_read(v) < 0)
        return MD_BADREQUEST;
    {
        int64_t t = v->pkt->pts != AV_NOPTS_VALUE ? v->pkt->pts : v->pkt->dts;
        v->start_us = t != AV_NOPTS_VALUE ? av_rescale_q(t, st->time_base, AV_TIME_BASE_Q)
                    : v->fmt->start_time != AV_NOPTS_VALUE ? v->fmt->start_time : 0;
        r = vq_push(v, v->pkt);
        av_packet_unref(v->pkt);
        if (r < 0)
            return MD_HOSTERROR;
    }
    /* Frames: from the length when the stream says it (a film on a web
     * server), else 0 (a live channel, which never ends). */
    v->frames = v->fmt->duration > 0 && v->fmt->duration != AV_NOPTS_VALUE
                ? (uint32_t)(v->fmt->duration * (*fps1000 / 1000.0) / 1000000) : 0;
    return MD_OK;
}

/* Decodes the live stream on to the frame for index's time and leaves it in
 * v->rgb. A frame asked for again, or one whose time has gone by, is the
 * one shown last: a live stream does not go back. */
static int live_decode(struct video *v, uint32_t index)
{
    int64_t target = frame_us(v, index), half = 500000LL * 1000 / (v->fps1000 ? v->fps1000 : 25000);
    AVRational tb = v->fmt->streams[v->stream]->time_base;
    int flushed = 0;

    if (v->shown != UINT32_MAX && target <= v->shown_us + half)
        return MD_OK;
    for (;;) {
        int r = avcodec_receive_frame(v->dec, v->frame);
        if (r == 0) {
            int64_t t = v->frame->best_effort_timestamp;
            int64_t us = t == AV_NOPTS_VALUE ? target : av_rescale_q(t, tb, AV_TIME_BASE_Q);
            if (us + half >= target || v->shown == UINT32_MAX) {
                if (!(v->sws = sws_getCachedContext(v->sws, v->frame->width, v->frame->height, v->frame->format,
                                                    v->ow, v->oh, AV_PIX_FMT_RGB24, SWS_AREA, NULL, NULL, NULL))) {
                    av_frame_unref(v->frame);
                    return MD_HOSTERROR;
                }
                sws_scale(v->sws, (const uint8_t *const *)v->frame->data, v->frame->linesize, 0, v->frame->height,
                          v->rgb->data, v->rgb->linesize);
                v->shown = index;
                v->shown_us = us;
                av_frame_unref(v->frame);
                if (us + half >= target)
                    return MD_OK;
                continue;
            }
            av_frame_unref(v->frame);
            continue;
        }
        if (r != AVERROR(EAGAIN))                    /* the end, after the last frame */
            return v->shown != UINT32_MAX ? MD_OK : MD_BADREQUEST;
        if (v->vq_n) {
            AVPacket *p = v->vq[v->vq_head];
            v->vq_head = (v->vq_head + 1) % v->vq_cap;
            v->vq_n--;
            avcodec_send_packet(v->dec, p);
            av_packet_free(&p);
        } else if (live_read(v) > 0) {
            avcodec_send_packet(v->dec, v->pkt);
            av_packet_unref(v->pkt);
        } else if (!flushed) {
            avcodec_send_packet(v->dec, NULL);
            flushed = 1;
        } else
            return v->shown != UINT32_MAX ? MD_OK : MD_BADREQUEST;
    }
}

/* ---- sound for VSOUND ---------------------------------------------------- */

static void track_put(struct track *t, const int16_t *pcm, int n)
{
    int i;

    if (t->next == INT64_MIN)
        t->next = t->first + (int64_t)t->len;
    if (!t->len)
        t->first = t->next < 0 ? 0 : t->next;
    for (i = 0; i < n; i++, t->next++) {
        int64_t at = t->next - t->first;
        if (at < 0)
            continue;                               /* before time zero */
        if ((size_t)at >= t->cap) {
            size_t cap = t->cap ? t->cap * 2 : 65536;
            int8_t *more;
            while (cap <= (size_t)at)
                cap *= 2;
            if (!(more = realloc(t->pcm, cap)))
                return;
            t->pcm = more;
            t->cap = cap;
        }
        if ((size_t)at > t->len)                    /* a gap: silence */
            memset(t->pcm + t->len, 0, (size_t)at - t->len);
        t->pcm[at] = (int8_t)(pcm[i] >> 8);
        if ((size_t)at >= t->len)
            t->len = (size_t)at + 1;
    }
}

static void track_drain(struct track *t, AVRational tb, int64_t start_us)
{
    while (avcodec_receive_frame(t->dec, t->frame) == 0) {
        int room = swr_get_out_samples(t->swr, t->frame->nb_samples), got;
        int16_t *pcm;
        if (t->next == INT64_MIN && t->frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            int64_t us = av_rescale_q(t->frame->best_effort_timestamp, tb, AV_TIME_BASE_Q) - start_us;
            t->next = us * (int64_t)t->rate / 1000000;
        }
        if (room > 0 && (pcm = av_malloc((size_t)room * 2)) != NULL) {
            got = swr_convert(t->swr, (uint8_t **)&pcm, room, (const uint8_t **)t->frame->extended_data,
                              t->frame->nb_samples);
            if (got > 0)
                track_put(t, pcm, got);
            av_free(pcm);
        }
        av_frame_unref(t->frame);
    }
}

static void track_take(struct video *v, struct track *t, AVPacket *pkt)
{
    AVFormatContext *fmt = t->fmt ? t->fmt : v->fmt;
    if (avcodec_send_packet(t->dec, pkt) >= 0)
        track_drain(t, fmt->streams[t->stream]->time_base, v->start_us);
}

static int track_open(struct video *v, uint32_t rate, struct track **out)
{
    const AVCodec *codec = NULL;
    AVFormatContext *fmt;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    struct track *t = calloc(1, sizeof *t);
    uint8_t *iobuf;

    if (!t)
        return MD_HOSTERROR;
    *out = t;
    t->rate = rate;
    t->next = INT64_MIN;
    if (v->live)
        fmt = v->fmt;
    else {
        t->mem.data = v->file;
        t->mem.size = v->mem.size;
        if (!(iobuf = av_malloc(65536)))
            return MD_HOSTERROR;
        if (!(t->io = avio_alloc_context(iobuf, 65536, 0, &t->mem, mem_read, NULL, mem_seek))) {
            av_free(iobuf);
            return MD_HOSTERROR;
        }
        if (!(t->fmt = avformat_alloc_context()))
            return MD_HOSTERROR;
        t->fmt->pb = t->io;
        if (avformat_open_input(&t->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(t->fmt, NULL) < 0)
            return MD_BADREQUEST;
        fmt = t->fmt;
    }
    if ((t->stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) < 0 || !codec)
        return MD_BADREQUEST;                       /* no sound */
    if (!(t->dec = avcodec_alloc_context3(codec))
        || avcodec_parameters_to_context(t->dec, fmt->streams[t->stream]->codecpar) < 0)
        return MD_HOSTERROR;
    if (avcodec_open2(t->dec, codec, NULL) < 0 || t->dec->sample_rate <= 0 || t->dec->ch_layout.nb_channels <= 0)
        return MD_BADREQUEST;
    if (swr_alloc_set_opts2(&t->swr, &mono, AV_SAMPLE_FMT_S16, rate, &t->dec->ch_layout, t->dec->sample_fmt,
                            t->dec->sample_rate, 0, NULL) < 0 || swr_init(t->swr) < 0)
        return MD_HOSTERROR;
    if (!(t->pkt = av_packet_alloc()) || !(t->frame = av_frame_alloc()))
        return MD_HOSTERROR;
    return MD_OK;
}

/* A file's sound track, from the sample at: sought there, its window emptied. */
static void track_seek(struct video *v, struct track *t, int64_t at)
{
    int64_t us = v->start_us + at * 1000000 / t->rate;
    av_seek_frame(t->fmt, -1, us > 0 ? us : 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(t->dec);
    swr_init(t->swr);
    t->len = 0;
    t->first = 0;
    t->next = INT64_MIN;
    t->eof = 0;
}

/* Decodes on until the window holds the sample before upto, or the sound ends. */
static void track_fill(struct video *v, struct track *t, int64_t upto)
{
    while (!t->eof && (!t->len || t->first + (int64_t)t->len < upto)) {
        if (t->fmt) {
            if (av_read_frame(t->fmt, t->pkt) < 0) {
                avcodec_send_packet(t->dec, NULL);
                track_drain(t, t->fmt->streams[t->stream]->time_base, v->start_us);
                t->eof = 1;
                break;
            }
            if (t->pkt->stream_index == t->stream)
                track_take(v, t, t->pkt);
            av_packet_unref(t->pkt);
        } else {
            /* Live: the pictures read so far wait in the queue. */
            if (v->vq_n >= LIVE_QUEUE || live_read(v) < 0) {
                t->eof = v->eof;
                break;
            }
            if (vq_push(v, v->pkt) < 0) {
                av_packet_unref(v->pkt);
                break;
            }
            av_packet_unref(v->pkt);
        }
    }
}

/* 1 when an animation asks to repeat: an APNG's acTL or a GIF's NETSCAPE2.0
 * block with a play count of 0 (for ever) or more than one. */
static int loops(const uint8_t *d, uint32_t n)
{
    uint32_t i, end = n < 65536 ? n : 65536;

    if (n > 8 && !memcmp(d, "\x89PNG", 4)) {
        for (i = 8; i + 8 <= end;) {                  /* chunks up to the first IDAT */
            uint32_t len = (uint32_t)d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3];
            if (!memcmp(d + i + 4, "acTL", 4) && i + 16 <= n) {
                uint32_t plays = (uint32_t)d[i + 12] << 24 | d[i + 13] << 16 | d[i + 14] << 8 | d[i + 15];
                return plays != 1;
            }
            if (!memcmp(d + i + 4, "IDAT", 4) || len > n)
                break;
            i += 12 + len;
        }
        return 0;
    }
    if (n > 6 && !memcmp(d, "GIF8", 4))
        for (i = 13; i + 16 <= end; i++)
            if (d[i] == 0x21 && d[i + 1] == 0xff && d[i + 2] == 11 && !memcmp(d + i + 3, "NETSCAPE2.0", 11)
                && d[i + 14] == 3 && d[i + 15] == 1 && i + 18 <= n)
                return (d[i + 16] | d[i + 17] << 8) != 1;
    return 0;
}

/* ---- uploads (VUPLOAD) --------------------------------------------------- */

/* A file sent in parts, for a VOPEN of a video too big for the Amiga to hold
 * at once. At most MAX_UPLOADS at a time (the oldest goes), each up to 1 GB. */
#define MAX_UPLOADS 4
#define MAX_UPLOAD (1u << 30)

struct upload {
    int used, done;
    uint32_t serial;
    uint8_t *data;
    size_t len, cap;
};

static struct upload uploads[MAX_UPLOADS];

static void upload_free(struct upload *u)
{
    free(u->data);
    memset(u, 0, sizeof *u);
}

int md_video_upload(uint32_t handle, const struct md_buffer *part, const uint32_t extra[4], uint32_t *result,
                    uint32_t *aux)
{
    struct upload *u = NULL;
    uint32_t i;
    int st = MD_OK;

    pthread_mutex_lock(&video_lock);
    if (!handle) {
        for (i = 0; i < MAX_UPLOADS && !u; i++)
            if (!uploads[i].used)
                u = &uploads[i];
        if (!u) {
            u = &uploads[0];
            for (i = 1; i < MAX_UPLOADS; i++)
                if (uploads[i].serial < u->serial)
                    u = &uploads[i];
            upload_free(u);
        }
        u->used = 1;
        u->serial = ++video_serial;
    } else if (handle > MAX_UPLOADS || !uploads[handle - 1].used || uploads[handle - 1].done) {
        pthread_mutex_unlock(&video_lock);
        return MD_BADREQUEST;
    } else
        u = &uploads[handle - 1];
    if (part->in && part->length) {
        if (u->len + part->length > MAX_UPLOAD)
            st = MD_BADREQUEST;
        else {
            if (u->len + part->length > u->cap) {
                size_t cap = u->cap ? u->cap : 1 << 20;
                uint8_t *more;
                while (cap < u->len + part->length)
                    cap *= 2;
                if (!(more = realloc(u->data, cap)))
                    st = MD_HOSTERROR;
                else {
                    u->data = more;
                    u->cap = cap;
                }
            }
            if (st == MD_OK) {
                memcpy(u->data + u->len, part->in, part->length);
                u->len += part->length;
            }
        }
    }
    if (st != MD_OK)
        upload_free(u);
    else {
        if (extra[0] & 1)
            u->done = 1;
        *result = (uint32_t)(u - uploads) + 1;
        *aux = (uint32_t)u->len;
    }
    pthread_mutex_unlock(&video_lock);
    return st;
}

/* ---- the calls ------------------------------------------------------------ */

static struct video *video_slot(void)
{
    struct video *v = NULL;
    uint32_t i;

    for (i = 0; i < MAX_VIDEOS && !v; i++)
        if (!videos[i].used)
            v = &videos[i];
    if (!v) {                                  /* push out the oldest */
        v = &videos[0];
        for (i = 1; i < MAX_VIDEOS; i++)
            if (videos[i].serial < v->serial)
                v = &videos[i];
        video_free(v);
    }
    v->used = 1;
    v->serial = ++video_serial;
    return v;
}

static void video_info(struct video *v, uint32_t flags, uint8_t info[MD_INFO_SIZE])
{
    AVFormatContext *fmt = v->fmt;
    int audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0;

    md_put32(info, MD_KIND_ANIMATION);
    md_put32(info + 4, video_fourcc(v->dec->codec_id));
    md_put32(info + 8, (audio ? MD_FLAG_SOUND : 0) | flags);
    md_put32(info + 12, v->frames);
    md_put32(info + 16, v->ow);
    md_put32(info + 20, v->oh);
}

int md_video_open(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                  uint32_t *result, uint32_t *aux)
{
    struct video *v;
    const uint8_t *d = file->in;
    uint32_t n = file->length, fps = 0;
    int st, own = 0;

    pthread_mutex_lock(&video_lock);
    if (!n) {                                  /* an upload, handed over to the video */
        struct upload *u;
        if (!extra[2] || extra[2] > MAX_UPLOADS || !(u = &uploads[extra[2] - 1])->used || !u->done || !u->len
            || md_is_av(u->data, (uint32_t)u->len) == MD_AV_NONE) {
            pthread_mutex_unlock(&video_lock);
            return MD_BADREQUEST;
        }
        d = u->data;
        n = (uint32_t)u->len;
        u->data = NULL;
        upload_free(u);
        own = 1;
    }
    v = video_slot();
    st = video_open(v, d, n, own, extra[0], extra[1], &fps);
    if (st != MD_OK) {
        if (own && !v->file)
            free((void *)d);
        video_free(v);
        pthread_mutex_unlock(&video_lock);
        return st;
    }
    v->fps1000 = fps;
    video_info(v, loops(v->file, n) ? MD_FLAG_LOOP : 0, info);
    *result = (uint32_t)(v - videos) + 1;
    *aux = fps;
    pthread_mutex_unlock(&video_lock);
    return MD_OK;
}

int md_video_open_url(const struct md_buffer *url, const uint32_t extra[4], uint8_t info[MD_INFO_SIZE],
                      uint32_t *result, uint32_t *aux)
{
    struct video *v;
    char text[2048];
    uint32_t n = url->length, fps = 0, i;
    int st;

    while (n && (url->in[n - 1] == 0 || url->in[n - 1] == '\n' || url->in[n - 1] == '\r' || url->in[n - 1] == ' '))
        n--;
    if (!n || n >= sizeof text)
        return MD_BADREQUEST;
    for (i = 0; i < n; i++) {
        if (url->in[i] < 0x21 || url->in[i] == 0x7f)
            return MD_BADREQUEST;               /* one URL, no spaces or controls */
        text[i] = (char)url->in[i];
    }
    text[n] = 0;
    if (strncmp(text, "http://", 7) && strncmp(text, "https://", 8))
        return MD_BADREQUEST;
    pthread_mutex_lock(&video_lock);
    v = video_slot();
    st = video_open_url(v, text, extra[0], extra[1], &fps);
    if (st != MD_OK) {
        video_free(v);
        pthread_mutex_unlock(&video_lock);
        return st;
    }
    v->fps1000 = fps;
    video_info(v, MD_FLAG_LIVE, info);
    *result = (uint32_t)(v - videos) + 1;
    *aux = fps;
    pthread_mutex_unlock(&video_lock);
    return MD_OK;
}

int md_video_frame(uint32_t handle, const uint32_t extra[4], struct md_buffer *out, uint32_t *result, uint32_t *aux)
{
    struct video *v;
    uint32_t want = extra[0], pixels;
    int st;

    if (!handle || handle > MAX_VIDEOS)
        return MD_BADREQUEST;
    pthread_mutex_lock(&video_lock);
    v = &videos[handle - 1];
    if (!v->used || extra[1] > 2) {
        pthread_mutex_unlock(&video_lock);
        return MD_BADREQUEST;
    }
    if (v->frames && want >= v->frames)
        want = v->frames - 1;
    pixels = v->ow * v->oh;
    if (!out->out || out->length < (extra[1] == 1 ? pixels * 3 : extra[1] == 2 ? HAM8_PALETTE + pixels : pixels)) {
        pthread_mutex_unlock(&video_lock);
        return MD_TOOSMALL;
    }
    if (v->live)
        st = live_decode(v, want);
    else {
        uint32_t src = v->map ? v->map[want] : want;
        st = src == v->shown ? MD_OK : video_seek_decode(v, src);
        v->shown = st == MD_OK ? src : UINT32_MAX;
    }
    if (st == MD_OK) {
        if (extra[1] == 1) {                       /* 24-bit RGB */
            uint32_t y;
            for (y = 0; y < v->oh; y++)
                memcpy(out->out + (size_t)y * v->ow * 3, v->rgb->data[0] + (size_t)y * v->rgb->linesize[0], v->ow * 3);
            out->written = pixels * 3;
        } else if (extra[1] == 2) {                /* HAM8: the base colours, then the pixels */
            ham8_palette(v->rgb->data[0], v->rgb->linesize[0], v->ow, v->oh, extra[2] & MD_HAM8_FIXED, out->out);
            ham8_map(v->rgb->data[0], v->rgb->linesize[0], v->ow, v->oh, out->out, out->out + HAM8_PALETTE);
            out->written = HAM8_PALETTE + pixels;
        } else {
            dither(v->rgb->data[0], v->rgb->linesize[0], v->ow, v->oh, out->out);
            out->written = pixels;
        }
        *result = want;
        *aux = 0;
    }
    pthread_mutex_unlock(&video_lock);
    return st;
}

int md_video_sound(uint32_t handle, const uint32_t extra[4], struct md_buffer *out, uint32_t *result, uint32_t *aux)
{
    struct video *v;
    struct track *t;
    uint32_t rate = extra[1] ? extra[1] : 22050, fps1000;
    int64_t a, b, k;
    int st;

    if (!handle || handle > MAX_VIDEOS || rate < 2000 || rate > 48000)
        return MD_BADREQUEST;
    pthread_mutex_lock(&video_lock);
    v = &videos[handle - 1];
    if (!v->used) {
        pthread_mutex_unlock(&video_lock);
        return MD_BADREQUEST;
    }
    if (v->sound && v->sound->rate != rate) {
        track_free(v->sound);
        v->sound = NULL;
    }
    if (!v->sound && (st = track_open(v, rate, &v->sound)) != MD_OK) {
        track_free(v->sound);
        v->sound = NULL;
        pthread_mutex_unlock(&video_lock);
        return st;
    }
    t = v->sound;
    fps1000 = v->fps1000 ? v->fps1000 : 25000;
    a = (int64_t)extra[0] * rate * 1000 / fps1000;
    b = ((int64_t)extra[0] + 1) * rate * 1000 / fps1000;
    if (!out->out || out->length < (uint64_t)(b - a)) {
        pthread_mutex_unlock(&video_lock);
        return MD_TOOSMALL;
    }
    /* A file's sound seeks when the frame asked for is behind the window or
     * well ahead of it; a live stream's only reads on. */
    if (t->fmt && t->len && (a < t->first || a > t->first + (int64_t)t->len + rate * 2))
        track_seek(v, t, a);
    else if (t->fmt && !t->len && a > rate * 2 && t->next == INT64_MIN)
        track_seek(v, t, a);
    track_fill(v, t, b);
    for (k = a; k < b; k++) {
        int64_t at = k - t->first;
        out->out[k - a] = at >= 0 && (size_t)at < t->len ? (uint8_t)t->pcm[at] : 0;
    }
    /* Keep a second before this frame, for one asked for again. */
    if (a - rate > t->first && (size_t)(a - rate - t->first) <= t->len) {
        size_t drop = (size_t)(a - rate - t->first);
        memmove(t->pcm, t->pcm + drop, t->len - drop);
        t->len -= drop;
        t->first += (int64_t)drop;
    }
    out->written = (uint32_t)(b - a);
    *result = (uint32_t)(b - a);
    *aux = rate;
    pthread_mutex_unlock(&video_lock);
    return MD_OK;
}

int md_video_close(uint32_t handle)
{
    if (!handle || handle > MAX_VIDEOS)
        return MD_BADREQUEST;
    pthread_mutex_lock(&video_lock);
    if (videos[handle - 1].used)
        video_free(&videos[handle - 1]);
    pthread_mutex_unlock(&video_lock);
    return MD_OK;
}
