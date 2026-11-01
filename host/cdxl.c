/*
 * media.cdxl/1 on the host (cdxl.h, docs/MEDIA_CDXL.md): a movie becomes a
 * CDXL file, the frames scaled to fit, quantised to a palette of their own
 * (median cut, ordered dither) or HAM-encoded, or 24-bit chunky for RTG,
 * with 8-bit sound cut to each frame. The result is kept in the cache and
 * handed out in pieces.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include "cdxl.h"
#include "hostrun.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define CX_MODE_COLOURS 0
#define CX_MODE_HAM     1
#define CX_MODE_CHUNKY  2

struct settings {
    uint32_t maxw, maxh, fps, planes, mode, rate, stereo, sound;
};

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }

static int settings(const uint32_t extra[4], struct settings *s)
{
    static const struct settings preset[] = {
        [CX_ECS]     = { 320, 180, 12, 5, CX_MODE_COLOURS, 11025, 0, 1 },
        [CX_ECS_HAM] = { 320, 180, 12, 6, CX_MODE_HAM,     11025, 0, 1 },
        [CX_AGA]     = { 320, 180, 15, 8, CX_MODE_COLOURS, 22050, 0, 1 },
        [CX_AGA_HAM] = { 320, 180, 15, 8, CX_MODE_HAM,     22050, 0, 1 },
        [CX_RTG]     = { 640, 360, 25, 24, CX_MODE_CHUNKY, 22050, 1, 1 },
    };
    uint32_t v;

    if (extra[0] > CX_RTG)
        return -1;
    *s = preset[extra[0]];
    if (extra[1]) {
        s->maxw = extra[1] >> 16;
        s->maxh = extra[1] & 0xffff;
    }
    if ((v = extra[2] & 0xff))
        s->fps = v;
    if ((v = (extra[2] >> 8) & 0xff) && s->mode == CX_MODE_COLOURS)
        s->planes = v;
    if ((v = extra[3] & 0x3ffff))
        s->rate = v;
    if (extra[3] & CX_STEREO)
        s->stereo = 1;
    if (extra[3] & CX_NO_SOUND)
        s->sound = 0;
    /* What the CDXL header and FFmpeg's reader allow. */
    if (s->maxw < 16 || s->maxh < 2 || s->maxw > 640 || s->maxh > 480 || !s->fps || s->fps > 60
        || s->planes < 1 || (s->mode != CX_MODE_CHUNKY && s->planes > 8) || s->rate < 2000 || s->rate > 44100)
        return -1;
    return 0;
}

/* ---- reading the movie ---------------------------------------------------- */

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

struct input {
    struct mem mem;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    int stream;
};

static void input_close(struct input *in)
{
    avcodec_free_context(&in->dec);
    avformat_close_input(&in->fmt);
    if (in->io) {
        av_freep(&in->io->buffer);
        avio_context_free(&in->io);
    }
}

static int input_open(struct input *in, const uint8_t *d, uint32_t n, enum AVMediaType type)
{
    const AVCodec *codec;
    uint8_t *iobuf;

    memset(in, 0, sizeof *in);
    in->mem.data = d;
    in->mem.size = n;
    av_log_set_level(AV_LOG_ERROR);
    if (!(iobuf = av_malloc(65536)))
        return -1;
    if (!(in->io = avio_alloc_context(iobuf, 65536, 0, &in->mem, mem_read, NULL, mem_seek))) {
        av_free(iobuf);
        return -1;
    }
    if (!(in->fmt = avformat_alloc_context()))
        goto fail;
    in->fmt->pb = in->io;
    if (avformat_open_input(&in->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(in->fmt, NULL) < 0)
        goto fail;
    if ((in->stream = av_find_best_stream(in->fmt, type, -1, -1, &codec, 0)) < 0)
        goto fail;
    if (!(in->dec = avcodec_alloc_context3(codec))
        || avcodec_parameters_to_context(in->dec, in->fmt->streams[in->stream]->codecpar) < 0
        || avcodec_open2(in->dec, codec, NULL) < 0)
        goto fail;
    return 0;
fail:
    input_close(in);
    return -1;
}

/* The whole sound as signed 8-bit, one array per channel. */
struct sound {
    int8_t *pcm[2];
    size_t frames, room;
};

static int sound_add(struct sound *s, uint8_t **planes, int n, int channels)
{
    int c, i;
    if (s->frames + n > s->room) {
        size_t room = (s->frames + n) * 2;
        for (c = 0; c < channels; c++) {
            int8_t *p = realloc(s->pcm[c], room);
            if (!p)
                return -1;
            s->pcm[c] = p;
        }
        s->room = room;
    }
    for (c = 0; c < channels; c++)
        for (i = 0; i < n; i++)
            s->pcm[c][s->frames + i] = (int8_t)(planes[c][i] ^ 0x80);   /* unsigned to signed */
    s->frames += n;
    return 0;
}

static void read_sound(const uint8_t *d, uint32_t n, const struct settings *st, struct sound *s)
{
    struct input in;
    SwrContext *swr = NULL;
    AVChannelLayout out_layout;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    uint8_t *planes[2] = { NULL, NULL };
    int channels = st->stereo ? 2 : 1, room = 0;

    memset(s, 0, sizeof *s);
    if (input_open(&in, d, n, AVMEDIA_TYPE_AUDIO))
        return;                                 /* a silent movie */
    av_channel_layout_default(&out_layout, channels);
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_U8P, (int)st->rate, &in.dec->ch_layout,
                            in.dec->sample_fmt, in.dec->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0)
        goto out;
    if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc()))
        goto out;
    for (;;) {
        int eof = av_read_frame(in.fmt, pkt) < 0, got;
        if (!eof && pkt->stream_index != in.stream) {
            av_packet_unref(pkt);
            continue;
        }
        avcodec_send_packet(in.dec, eof ? NULL : pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(in.dec, frame) >= 0) {
            int want = swr_get_out_samples(swr, frame->nb_samples);
            if (want > room) {
                free(planes[0]);
                free(planes[1]);
                planes[0] = malloc(want);
                planes[1] = malloc(want);
                room = want;
                if (!planes[0] || !planes[1])
                    goto out;
            }
            got = swr_convert(swr, planes, room, (const uint8_t **)frame->extended_data, frame->nb_samples);
            if (got > 0 && sound_add(s, planes, got, channels))
                goto out;
        }
        if (eof)
            break;
    }
    if (room) {
        int got = swr_convert(swr, planes, room, NULL, 0);
        if (got > 0)
            sound_add(s, planes, got, channels);
    }
out:
    free(planes[0]);
    free(planes[1]);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    input_close(&in);
}

/* ---- colours -------------------------------------------------------------- */

static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

struct colour { uint16_t rgb; uint32_t count; };     /* rgb: 12-bit, 0xRGB */

static int sort_axis;
static int by_axis(const void *a, const void *b)
{
    int sh = 8 - 4 * sort_axis;
    return (int)((((const struct colour *)a)->rgb >> sh) & 15) - (int)((((const struct colour *)b)->rgb >> sh) & 15);
}

/* Median cut of a 12-bit histogram into at most want colours (12-bit). */
static int median_cut(const uint32_t hist[4096], int want, uint16_t *pal)
{
    struct colour col[4096];
    int start[256], end[256], boxes = 1, n = 0, i, b;

    for (i = 0; i < 4096; i++)
        if (hist[i]) {
            col[n].rgb = (uint16_t)i;
            col[n++].count = hist[i];
        }
    if (!n) {
        pal[0] = 0;
        return 1;
    }
    start[0] = 0;
    end[0] = n;
    while (boxes < want) {
        int best = -1, best_axis = 0;
        uint64_t best_score = 0;
        for (b = 0; b < boxes; b++) {
            int lo[3] = { 15, 15, 15 }, hi[3] = { 0, 0, 0 }, a;
            uint64_t count = 0;
            if (end[b] - start[b] < 2)
                continue;
            for (i = start[b]; i < end[b]; i++)
                for (a = 0; a < 3; a++) {
                    int v = (col[i].rgb >> (8 - 4 * a)) & 15;
                    lo[a] = v < lo[a] ? v : lo[a];
                    hi[a] = v > hi[a] ? v : hi[a];
                }
            for (i = start[b]; i < end[b]; i++)
                count += col[i].count;
            for (a = 0; a < 3; a++)
                if ((uint64_t)(hi[a] - lo[a]) * count > best_score) {
                    best_score = (uint64_t)(hi[a] - lo[a]) * count;
                    best = b;
                    best_axis = a;
                }
        }
        if (best < 0)
            break;
        {
            uint64_t total = 0, run = 0;
            int mid;
            sort_axis = best_axis;
            qsort(col + start[best], end[best] - start[best], sizeof col[0], by_axis);
            for (i = start[best]; i < end[best]; i++)
                total += col[i].count;
            for (mid = start[best]; mid < end[best] - 1; mid++) {
                run += col[mid].count;
                if (run * 2 >= total)
                    break;
            }
            if (mid == end[best] - 1)
                mid--;                              /* one heavy colour at the end: keep both halves */
            start[boxes] = mid + 1;
            end[boxes] = end[best];
            end[best] = mid + 1;
            boxes++;
        }
    }
    for (b = 0; b < boxes; b++) {
        uint64_t sum[3] = { 0, 0, 0 }, count = 0;
        for (i = start[b]; i < end[b]; i++) {
            sum[0] += (uint64_t)((col[i].rgb >> 8) & 15) * col[i].count;
            sum[1] += (uint64_t)((col[i].rgb >> 4) & 15) * col[i].count;
            sum[2] += (uint64_t)(col[i].rgb & 15) * col[i].count;
            count += col[i].count;
        }
        pal[b] = (uint16_t)((sum[0] + count / 2) / count << 8 | (sum[1] + count / 2) / count << 4
                            | (sum[2] + count / 2) / count);
    }
    return boxes;
}

static void histogram(const uint8_t *rgb, int w, int h, uint32_t hist[4096])
{
    int i;
    memset(hist, 0, 4096 * sizeof hist[0]);
    for (i = 0; i < w * h; i++, rgb += 3)
        hist[(rgb[0] >> 4) << 8 | (rgb[1] >> 4) << 4 | rgb[2] >> 4]++;
}

/* 8-bit channel from a palette's 4-bit one. */
#define X4(v) ((v) * 17)

static int dist(int r1, int g1, int b1, int r2, int g2, int b2)
{
    return 3 * (r1 - r2) * (r1 - r2) + 4 * (g1 - g2) * (g1 - g2) + 2 * (b1 - b2) * (b1 - b2);
}

/* Each pixel as a palette index, with a 4x4 ordered dither. */
static void map_colours(const uint8_t *rgb, int w, int h, const uint16_t *pal, int n, uint8_t *idx)
{
    uint8_t lut[4096];
    int i, x, y, k;

    for (i = 0; i < 4096; i++) {
        int r = X4(i >> 8), g = X4((i >> 4) & 15), b = X4(i & 15), best = 0, bd = 1 << 30;
        for (k = 0; k < n; k++) {
            int d = dist(r, g, b, X4(pal[k] >> 8), X4((pal[k] >> 4) & 15), X4(pal[k] & 15));
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        lut[i] = (uint8_t)best;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++, rgb += 3) {
            int d = (bayer[y & 3][x & 3] * 2 - 15) * 17 / 16, q[3], c;    /* about one 4-bit step */
            for (c = 0; c < 3; c++) {
                int v = rgb[c] + d;
                q[c] = (v < 0 ? 0 : v > 255 ? 255 : v) >> 4;
            }
            *idx++ = lut[q[0] << 8 | q[1] << 4 | q[2]];
        }
}

/* Hold-and-modify: each pixel is a base colour or the last pixel with one
 * channel changed, whichever is nearer. bits is 6 (HAM6) or 8 (HAM8). */
static void map_ham(const uint8_t *rgb, int w, int h, const uint16_t *pal, int n, int bits, uint8_t *idx)
{
    int x, y, k, shift = bits - 2, keep = bits == 8 ? 3 : 0;

    for (y = 0; y < h; y++) {
        int pr = X4(pal[0] >> 8), pg = X4((pal[0] >> 4) & 15), pb = X4(pal[0] & 15);
        for (x = 0; x < w; x++, rgb += 3) {
            int tr = rgb[0], tg = rgb[1], tb = rgb[2], best = 0, bd = 1 << 30, d, v[3], c;
            int nr = pr, ng = pg, nb = pb;
            for (k = 0; k < n; k++) {
                d = dist(tr, tg, tb, X4(pal[k] >> 8), X4((pal[k] >> 4) & 15), X4(pal[k] & 15));
                if (d < bd) {
                    bd = d;
                    best = k;
                    nr = X4(pal[k] >> 8);
                    ng = X4((pal[k] >> 4) & 15);
                    nb = X4(pal[k] & 15);
                }
            }
            /* The channel value a modify gives: HAM6 sets 4 bits (x 17), HAM8 the top 6. */
            for (c = 0; c < 3; c++) {
                int t = c == 0 ? tr : c == 1 ? tg : tb, p = c == 0 ? pr : c == 1 ? pg : pb;
                v[c] = bits == 6 ? t >> 4 : t >> 2;
                {
                    int got = bits == 6 ? X4(v[c]) : (v[c] << 2) | (p & keep);
                    int r2 = c == 0 ? got : pr, g2 = c == 1 ? got : pg, b2 = c == 2 ? got : pb;
                    d = dist(tr, tg, tb, r2, g2, b2);
                    if (d < bd) {
                        static const int op[3] = { 2, 3, 1 };      /* red, green, blue */
                        bd = d;
                        best = op[c] << shift | v[c];
                        nr = r2;
                        ng = g2;
                        nb = b2;
                    }
                }
            }
            *idx++ = (uint8_t)best;
            pr = nr;
            pg = ng;
            pb = nb;
        }
    }
}

/* Chunky indices to bitplanes, plane after plane, rows padded to 16 bits. */
static void to_planes(const uint8_t *idx, int w, int h, int planes, uint8_t *out)
{
    int rowbytes = (w + 15) / 16 * 2, p, x, y;
    memset(out, 0, (size_t)rowbytes * h * planes);
    for (p = 0; p < planes; p++)
        for (y = 0; y < h; y++) {
            uint8_t *row = out + ((size_t)p * h + y) * rowbytes;
            const uint8_t *in = idx + (size_t)y * w;
            for (x = 0; x < w; x++)
                if ((in[x] >> p) & 1)
                    row[x >> 3] |= 0x80 >> (x & 7);
        }
}

/* ---- writing -------------------------------------------------------------- */

struct writer {
    FILE *f;
    const struct settings *st;
    int w, h, spf, rate;
    uint32_t frames, prev;
    const struct sound *sound;
    uint8_t *idx, *planes;
};

static int write_frame(struct writer *wr, const uint8_t *rgb)
{
    const struct settings *st = wr->st;
    uint16_t pal[256];
    uint32_t hist[4096];
    uint8_t head[32], palbytes[512];
    int n = 0, i, c, channels = st->stereo ? 2 : 1;
    size_t image, size, audio = st->sound ? (size_t)wr->spf : 0;

    if (st->mode == CX_MODE_CHUNKY) {
        image = (size_t)wr->w * wr->h * 3;
    } else {
        int colours = st->mode == CX_MODE_HAM ? 1 << (st->planes - 2) : 1 << st->planes;
        histogram(rgb, wr->w, wr->h, hist);
        n = median_cut(hist, colours, pal);
        for (i = n; i < colours; i++)
            pal[i] = 0;
        n = colours;                                  /* the palette is always full size */
        if (st->mode == CX_MODE_HAM)
            map_ham(rgb, wr->w, wr->h, pal, n, st->planes, wr->idx);
        else
            map_colours(rgb, wr->w, wr->h, pal, n, wr->idx);
        to_planes(wr->idx, wr->w, wr->h, st->planes, wr->planes);
        for (i = 0; i < n; i++)
            put16(palbytes + 2 * i, pal[i]);
        image = (size_t)(wr->w + 15) / 16 * 2 * wr->h * st->planes;
    }
    size = 32 + 2 * n + image + audio * channels;
    memset(head, 0, sizeof head);
    head[0] = 1;                                      /* standard: 12-bit palette */
    head[1] = (st->mode == CX_MODE_HAM ? 1 : 0) | (st->mode == CX_MODE_CHUNKY ? 0x20 : 0)
              | (audio && channels == 2 ? 0x10 : 0);
    put32(head + 2, (uint32_t)size);
    put32(head + 6, wr->prev);
    put32(head + 10, wr->frames + 1);
    put16(head + 14, wr->w);
    put16(head + 16, wr->h);
    head[19] = (uint8_t)st->planes;
    put16(head + 20, 2 * n);
    put16(head + 22, (uint32_t)audio);
    put16(head + 24, audio ? wr->rate : 0);
    head[26] = (uint8_t)st->fps;
    if (fwrite(head, 1, 32, wr->f) != 32 || (n && fwrite(palbytes, 1, 2 * n, wr->f) != (size_t)(2 * n)))
        return -1;
    if (st->mode == CX_MODE_CHUNKY ? fwrite(rgb, 1, image, wr->f) != image
                                   : fwrite(wr->planes, 1, image, wr->f) != image)
        return -1;
    for (c = 0; audio && c < channels; c++) {           /* left, then right */
        size_t from = (size_t)wr->frames * audio, k;
        for (k = 0; k < audio; k++) {
            int8_t v = from + k < wr->sound->frames ? wr->sound->pcm[c][from + k] : 0;
            if (fputc((uint8_t)v, wr->f) == EOF)
                return -1;
        }
    }
    wr->prev = (uint32_t)size;
    wr->frames++;
    return 0;
}

/* The picture's size inside the box, keeping its shape: width a multiple of 16. */
static void fit(int sw, int sh, AVRational sar, const struct settings *st, int *w, int *h)
{
    double aspect = (double)sw / sh * (sar.num > 0 && sar.den > 0 ? (double)sar.num / sar.den : 1.0);
    double fw = st->maxw, fh = fw / aspect;
    if (fh > st->maxh) {
        fh = st->maxh;
        fw = fh * aspect;
    }
    *w = ((int)(fw + 0.5) / 16) * 16;
    *h = ((int)(fh + 0.5) / 2) * 2;
    if (*w < 16)
        *w = 16;
    if (*h < 2)
        *h = 2;
}

static int transcode(const uint8_t *d, uint32_t n, const struct settings *st, const char *path, uint8_t info[CX_INFO_SIZE])
{
    struct input in;
    struct writer wr;
    struct sound sound;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    uint8_t *cur = NULL;
    double fps_src, last_t = 0;
    int64_t start = AV_NOPTS_VALUE;
    int have = 0, rc = -1, stride;
    AVRational tb;

    memset(&wr, 0, sizeof wr);
    if (input_open(&in, d, n, AVMEDIA_TYPE_VIDEO))
        return -1;
    memset(&sound, 0, sizeof sound);
    wr.st = st;
    wr.sound = &sound;
    if (st->sound) {
        wr.spf = ((int)st->rate / (int)st->fps + 1) & ~1;    /* even: Paula plays words */
        wr.rate = wr.spf * (int)st->fps;
        if (wr.rate > 65535) {
            wr.spf = (65535 / (int)st->fps) & ~1;
            wr.rate = wr.spf * (int)st->fps;
        }
        {
            struct settings at = *st;
            at.rate = (uint32_t)wr.rate;
            read_sound(d, n, &at, &sound);
        }
    }
    fit(in.dec->width, in.dec->height, in.fmt->streams[in.stream]->sample_aspect_ratio.num
        ? in.fmt->streams[in.stream]->sample_aspect_ratio : in.dec->sample_aspect_ratio, st, &wr.w, &wr.h);
    stride = wr.w * 3;
    tb = in.fmt->streams[in.stream]->time_base;
    {
        AVRational fr = av_guess_frame_rate(in.fmt, in.fmt->streams[in.stream], NULL);
        fps_src = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 25.0;
    }
    if (!(wr.f = fopen(path, "wb")) || !(cur = malloc((size_t)stride * wr.h)) || !(wr.idx = malloc((size_t)wr.w * wr.h))
        || !(wr.planes = malloc((size_t)(wr.w + 15) / 16 * 2 * wr.h * 8)) || !(pkt = av_packet_alloc())
        || !(frame = av_frame_alloc()))
        goto out;
    for (;;) {
        int eof = av_read_frame(in.fmt, pkt) < 0;
        if (!eof && pkt->stream_index != in.stream) {
            av_packet_unref(pkt);
            continue;
        }
        avcodec_send_packet(in.dec, eof ? NULL : pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(in.dec, frame) >= 0) {
            double t;
            int64_t ts = frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                t = have ? last_t + 1.0 / fps_src : 0;
            else {
                if (start == AV_NOPTS_VALUE)
                    start = ts;
                t = (ts - start) * av_q2d(tb);
            }
            /* Frames due before this one show the one we hold. */
            while (have && (double)wr.frames / st->fps < t - 1e-6)
                if (write_frame(&wr, cur))
                    goto out;
            if (!(sws = sws_getCachedContext(sws, frame->width, frame->height, frame->format, wr.w, wr.h,
                                             AV_PIX_FMT_RGB24, SWS_AREA, NULL, NULL, NULL)))
                goto out;
            sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, &cur, &stride);
            have = 1;
            last_t = t;
        }
        if (eof)
            break;
    }
    if (!have)
        goto out;
    /* The last picture holds for its own time (and a movie of one picture still gets it). */
    while (!wr.frames || (double)wr.frames / st->fps < last_t + 1.0 / fps_src - 1e-6)
        if (write_frame(&wr, cur))
            goto out;
    put32(info, 0x4344584cu);                        /* 'CDXL' */
    put32(info + 4, wr.frames);
    put32(info + 8, (uint32_t)wr.w);
    put32(info + 12, (uint32_t)wr.h);
    put32(info + 16, st->planes | st->mode << 8 | (st->sound && st->stereo ? 1u << 16 : 0));
    put32(info + 20, st->fps << 24 | (st->sound ? (uint32_t)wr.rate : 0));
    rc = 0;
out:
    if (wr.f && fclose(wr.f) && !rc)
        rc = -1;
    free(cur);
    free(wr.idx);
    free(wr.planes);
    free(sound.pcm[0]);
    free(sound.pcm[1]);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    input_close(&in);
    return rc;
}

/* ---- the service ---------------------------------------------------------- */

#define MAX_OUTPUTS 8

static struct output {
    int used;
    uint32_t serial;
    char path[700];
    uint32_t size;
} outputs[MAX_OUTPUTS];
static uint32_t output_serial;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

static int convert(const struct md_buffer *file, const uint32_t extra[4], uint8_t info[CX_INFO_SIZE],
                   uint32_t *handle, uint32_t *bytes)
{
    struct settings st;
    char dir[512], k[64], path[700], ipath[720], tmp[740];
    struct stat sb;
    uint8_t *saved;
    size_t len;
    int i, oldest = 0;

    if (!file->in || !file->length || settings(extra, &st))
        return MD_BADREQUEST;
    if (hr_cache_dir("cdxl", dir, sizeof dir))
        return MD_HOSTERROR;
    hr_key(file->in, file->length, k, sizeof k);
    snprintf(path, sizeof path, "%s/%s-%08x%08x%08x%08x.cdxl", dir, k, extra[0], extra[1], extra[2], extra[3]);
    snprintf(ipath, sizeof ipath, "%s.info", path);
    if ((saved = hr_read(ipath, &len)) && len == CX_INFO_SIZE && stat(path, &sb) == 0) {
        memcpy(info, saved, CX_INFO_SIZE);
        free(saved);
    } else {
        free(saved);
        snprintf(tmp, sizeof tmp, "%s.%d.part", path, (int)getpid());
        if (transcode(file->in, file->length, &st, tmp, info)) {
            unlink(tmp);
            return MD_BADREQUEST;
        }
        if (rename(tmp, path) || hr_write(ipath, info, CX_INFO_SIZE) || stat(path, &sb))
            return MD_HOSTERROR;
    }
    if (sb.st_size > 0xffffffffLL)
        return MD_BADREQUEST;
    pthread_mutex_lock(&output_lock);
    for (i = 0; i < MAX_OUTPUTS; i++) {
        if (!outputs[i].used)
            break;
        if (outputs[i].serial < outputs[oldest].serial)
            oldest = i;
    }
    if (i == MAX_OUTPUTS)
        i = oldest;                                     /* the least recently opened goes */
    outputs[i].used = 1;
    outputs[i].serial = ++output_serial;
    snprintf(outputs[i].path, sizeof outputs[i].path, "%s", path);
    outputs[i].size = (uint32_t)sb.st_size;
    pthread_mutex_unlock(&output_lock);
    *handle = (uint32_t)i + 1;
    *bytes = (uint32_t)sb.st_size;
    return MD_OK;
}

static int read_out(uint32_t handle, uint32_t offset, struct md_buffer *out, uint32_t *result)
{
    char path[700];
    uint32_t size, n;
    FILE *f;

    pthread_mutex_lock(&output_lock);
    if (!handle || handle > MAX_OUTPUTS || !outputs[handle - 1].used) {
        pthread_mutex_unlock(&output_lock);
        return MD_BADREQUEST;
    }
    snprintf(path, sizeof path, "%s", outputs[handle - 1].path);
    size = outputs[handle - 1].size;
    pthread_mutex_unlock(&output_lock);
    if (!out->out || !out->length)
        return MD_TOOSMALL;
    if (offset >= size) {
        *result = 0;
        return MD_OK;
    }
    n = size - offset < out->length ? size - offset : out->length;
    if (!(f = fopen(path, "rb")))
        return MD_HOSTERROR;
    if (fseek(f, offset, SEEK_SET) || fread(out->out, 1, n, f) != n) {
        fclose(f);
        return MD_HOSTERROR;
    }
    fclose(f);
    out->written = n;
    *result = n;
    return MD_OK;
}

int cx_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux)
{
    int st;

    *result = *aux = 0;
    switch (op) {
    case CX_CONVERT:
        if (!buf[1].out || buf[1].length < CX_INFO_SIZE)
            return MD_TOOSMALL;
        if ((st = convert(&buf[0], extra, buf[1].out, result, aux)) == MD_OK)
            buf[1].written = CX_INFO_SIZE;
        return st;
    case CX_READ:
        return read_out(arg, extra[0], &buf[1], result);
    case CX_CLOSE:
        pthread_mutex_lock(&output_lock);
        if (arg && arg <= MAX_OUTPUTS)
            outputs[arg - 1].used = 0;
        pthread_mutex_unlock(&output_lock);
        return MD_OK;
    default:
        return MD_BADREQUEST;
    }
}
