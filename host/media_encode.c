/*
 * media.encode/1 on the host (media_encode.h, docs/MEDIA_ENCODE.md): the
 * Amiga's pixels, samples and frames go into files in a private folder, and
 * FINISH runs the ffmpeg program on them in its own process: HEVC (x265)
 * for HEIC pictures and HEVC films, AAC and WMA 2 for sound, WMV 8 for
 * Windows Media films. This library never links FFmpeg, so its encoders and
 * their licences stay in that program. The result is handed out in pieces
 * by READ.
 *
 *   OPENSERVICE_FFMPEG  the ffmpeg program (default: ffmpeg on PATH)
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include "media_encode.h"
#include "heif_mux.h"
#include "hostrun.h"

#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum kind { K_PICTURE, K_SOUND, K_FILM };

struct job {
    uint32_t id;                   /* 0: no job by that number (a free slot unless running) */
    int busy;                      /* a call is working on it: not evicted */
    int started;                   /* FINISH has started FFmpeg on it */
    int running;                   /* FFmpeg is at work (its thread has it) */
    int closed;                    /* CLOSE came while it ran: the thread frees it */
    int finished;                  /* the file is there */
    int status;                    /* how FFmpeg's work went, once started and not running */
    enum kind kind;
    uint32_t format, width, height, pixels, fps1000, rate, channels, samples, quality, flags;
    char dir[256];
    FILE *stream[2];
    uint64_t bytes[2];
    uint64_t size;                 /* the file's, once finished */
    uint32_t frames, sample_frames;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
static struct job jobs[ME_MAX_JOBS];
static uint32_t serial;

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static const char *ffmpeg(void)
{
    const char *f = getenv("OPENSERVICE_FFMPEG");
    return f && *f ? f : "ffmpeg";
}

/* ---- what this host's FFmpeg can write ------------------------------------ */

/* Is name (a word) the second word of a line of list? (ffmpeg -encoders and
 * -muxers: flags, then the name.) */
static int listed(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *line = list; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        const char *p = line;
        while (*p == ' ')
            p++;
        while (*p && *p != ' ' && *p != '\n')            /* the flags */
            p++;
        while (*p == ' ')
            p++;
        if (!strncmp(p, name, n) && (p[n] == ' ' || p[n] == '\n' || !p[n]))
            return 1;
    }
    return 0;
}

static struct {
    char program[512];
    uint32_t list[8];
    uint32_t count;
    int known;
} offered;

/* The formats, asked of the ffmpeg program once (again if it changes). With lock held. */
static void find_formats(void)
{
    const char *prog = ffmpeg();
    char *enc, *mux, *argv[4];
    int x265, aac, wmav2, wmv2, ipod, adts, asf, mp4, hevc;

    if (offered.known && !strcmp(offered.program, prog))
        return;
    snprintf(offered.program, sizeof offered.program, "%s", prog);
    offered.known = 1;
    offered.count = 0;
    argv[0] = (char *)prog; argv[1] = "-hide_banner"; argv[2] = "-encoders"; argv[3] = NULL;
    enc = hr_run_read(argv, NULL);
    argv[2] = "-muxers";
    mux = hr_run_read(argv, NULL);
    if (enc && mux) {
        x265 = listed(enc, "libx265");
        aac = listed(enc, "aac");
        wmav2 = listed(enc, "wmav2");
        wmv2 = listed(enc, "wmv2");
        ipod = listed(mux, "ipod");
        adts = listed(mux, "adts");
        asf = listed(mux, "asf");
        mp4 = listed(mux, "mp4");
        hevc = listed(mux, "hevc");
        if (x265 && hevc)
            offered.list[offered.count++] = ME_FORMAT_HEIC;
        if (aac && ipod)
            offered.list[offered.count++] = ME_FORMAT_M4A;
        if (aac && adts)
            offered.list[offered.count++] = ME_FORMAT_AAC;
        if (wmav2 && asf)
            offered.list[offered.count++] = ME_FORMAT_WMA;
        if (x265 && aac && mp4)
            offered.list[offered.count++] = ME_FORMAT_HEVC;
        if (wmv2 && wmav2 && asf)
            offered.list[offered.count++] = ME_FORMAT_WMV;
    }
    free(enc);
    free(mux);
}

static int is_offered(uint32_t format)
{
    for (uint32_t i = 0; i < offered.count; i++)
        if (offered.list[i] == format)
            return 1;
    return 0;
}

/* ---- jobs ------------------------------------------------------------------- */

/* With lock held. */
static struct job *find(uint32_t id)
{
    for (int i = 0; i < ME_MAX_JOBS; i++)
        if (id && jobs[i].id == id)
            return &jobs[i];
    return NULL;
}

static void job_free(struct job *j)
{
    for (int s = 0; s < 2; s++)
        if (j->stream[s])
            fclose(j->stream[s]);
    if (j->dir[0])
        hr_rmdir(j->dir);
    memset(j, 0, sizeof *j);
}

/* Takes the job for one call; NULL when it is not there or busy. */
static struct job *take(uint32_t id)
{
    struct job *j;
    pthread_mutex_lock(&lock);
    if ((j = find(id)) && !j->busy)
        j->busy = 1;
    else
        j = NULL;
    pthread_mutex_unlock(&lock);
    return j;
}

static void give(struct job *j)
{
    pthread_mutex_lock(&lock);
    j->busy = 0;
    pthread_mutex_unlock(&lock);
}

static enum kind kind_of(uint32_t format)
{
    switch (format) {
    case ME_FORMAT_HEIC:
        return K_PICTURE;
    case ME_FORMAT_HEVC: case ME_FORMAT_WMV:
        return K_FILM;
    default:
        return K_SOUND;
    }
}

static uint32_t pixel_bytes(const struct job *j) { return j->pixels == ME_PIX_ARGB ? 4 : 3; }
static uint32_t frame_bytes(const struct job *j) { return j->width * j->height * pixel_bytes(j); }
static uint32_t sample_bytes(const struct job *j) { return j->channels * (j->samples == ME_SAMPLE_S16 ? 2 : 1); }

static int begin(const struct md_buffer *b, uint32_t *result)
{
    struct job s, *j = NULL;
    const uint8_t *p = b->in;
    char path[300];

    if (!p || b->length < ME_SETTINGS_SIZE || get32(p + ME_S_VERSION) != 1)
        return MD_BADREQUEST;
    memset(&s, 0, sizeof s);
    s.format = get32(p + ME_S_FORMAT);
    s.width = get32(p + ME_S_WIDTH);
    s.height = get32(p + ME_S_HEIGHT);
    s.pixels = get32(p + ME_S_PIXELS);
    s.fps1000 = get32(p + ME_S_FPS1000);
    s.rate = get32(p + ME_S_RATE);
    s.channels = get32(p + ME_S_CHANNELS);
    s.samples = get32(p + ME_S_SAMPLES);
    s.quality = get32(p + ME_S_QUALITY);
    s.flags = get32(p + ME_S_FLAGS);
    s.kind = kind_of(s.format);
    if (s.kind == K_PICTURE)
        s.rate = s.channels = 0;
    if (s.quality > 100 || s.pixels > ME_PIX_RGB || s.samples > ME_SAMPLE_S8)
        return MD_BADREQUEST;
    if (s.kind == K_PICTURE && (!s.width || !s.height || s.width > ME_MAX_SIDE || s.height > ME_MAX_SIDE))
        return MD_BADREQUEST;
    if (s.kind == K_FILM && (s.width < 2 || s.height < 2 || s.width > ME_MAX_FILM_SIDE
                             || s.height > ME_MAX_FILM_SIDE || s.fps1000 < 1000 || s.fps1000 > 120000))
        return MD_BADREQUEST;
    if ((s.kind == K_SOUND || s.rate) && (s.rate < 1000 || s.rate > 192000 || s.channels < 1 || s.channels > 2))
        return MD_BADREQUEST;

    pthread_mutex_lock(&lock);
    find_formats();
    if (!is_offered(s.format)) {
        pthread_mutex_unlock(&lock);
        return MD_BADREQUEST;
    }
    for (int i = 0; i < ME_MAX_JOBS && !j; i++)
        if (!jobs[i].id && !jobs[i].running)
            j = &jobs[i];
    if (!j)                                               /* the oldest that nothing is working on */
        for (int i = 0; i < ME_MAX_JOBS; i++)
            if (!jobs[i].busy && !jobs[i].running && (!j || jobs[i].id < j->id))
                j = &jobs[i];
    if (!j) {
        pthread_mutex_unlock(&lock);
        return MD_HOSTERROR;
    }
    if (j->id)
        job_free(j);
    *j = s;
    j->id = ++serial ? serial : ++serial;
    j->busy = 1;
    pthread_mutex_unlock(&lock);

    if (hr_tempdir(j->dir, sizeof j->dir)) {
        j->dir[0] = 0;
        goto fail;
    }
    if (j->kind != K_SOUND) {
        snprintf(path, sizeof path, "%s/pixels.raw", j->dir);
        if (!(j->stream[ME_STREAM_PICTURE] = fopen(path, "wb")))
            goto fail;
    }
    if (j->rate) {
        snprintf(path, sizeof path, "%s/sound.raw", j->dir);
        if (!(j->stream[ME_STREAM_SOUND] = fopen(path, "wb")))
            goto fail;
    }
    *result = j->id;
    give(j);
    return MD_OK;
fail:
    pthread_mutex_lock(&lock);
    job_free(j);
    pthread_mutex_unlock(&lock);
    return MD_HOSTERROR;
}

static int put_bytes(struct job *j, uint32_t stream, const struct md_buffer *b, uint32_t *result)
{
    if (stream > ME_STREAM_SOUND || !j->stream[stream] || j->started)
        return MD_BADREQUEST;
    if (b->in && b->length) {
        uint64_t limit = j->kind == K_PICTURE && stream == ME_STREAM_PICTURE ? frame_bytes(j) : ME_MAX_STREAM;
        if (j->bytes[stream] + b->length > limit)
            return MD_BADREQUEST;
        if (fwrite(b->in, 1, b->length, j->stream[stream]) != b->length)
            return MD_HOSTERROR;
        j->bytes[stream] += b->length;
    }
    *result = (uint32_t)j->bytes[stream];
    return MD_OK;
}

/* ---- FFmpeg ------------------------------------------------------------------- */

struct args {
    char *v[96];
    int n;
    char text[4096];
    size_t used;
};

static void arg(struct args *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void arg(struct args *a, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (a->n >= (int)(sizeof a->v / sizeof a->v[0]) - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(a->text + a->used, sizeof a->text - a->used, fmt, ap);
    va_end(ap);
    if (n < 0 || a->used + n + 1 > sizeof a->text)
        return;
    a->v[a->n++] = a->text + a->used;
    a->v[a->n] = NULL;
    a->used += n + 1;
}

static void start(struct args *a)
{
    memset(a, 0, sizeof *a);
    arg(a, "%s", ffmpeg());
    arg(a, "-nostdin");
    arg(a, "-hide_banner");
    arg(a, "-v");
    arg(a, "error");
    arg(a, "-y");
}

/* The raw pixels as an input. */
static void pixels_in(struct args *a, const struct job *j)
{
    arg(a, "-f"); arg(a, "rawvideo");
    arg(a, "-pix_fmt"); arg(a, "%s", j->pixels == ME_PIX_ARGB ? "argb" : "rgb24");
    arg(a, "-s"); arg(a, "%ux%u", j->width, j->height);
    if (j->kind == K_FILM) {
        arg(a, "-framerate"); arg(a, "%u/1000", j->fps1000);
    }
    arg(a, "-i"); arg(a, "%s/pixels.raw", j->dir);
}

static void sound_in(struct args *a, const struct job *j)
{
    arg(a, "-f"); arg(a, "%s", j->samples == ME_SAMPLE_S16 ? "s16be" : "s8");
    arg(a, "-ar"); arg(a, "%u", j->rate);
    arg(a, "-ac"); arg(a, "%u", j->channels);
    arg(a, "-i"); arg(a, "%s/sound.raw", j->dir);
}

/* AAC and WMA take the usual rates only: the nearest at or above the
 * sound's (28000 Hz, an Amiga's, becomes 32000), at most 48000. */
static uint32_t coded_rate(uint32_t rate)
{
    static const uint32_t rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000 };
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++)
        if (rate <= rates[i])
            return rates[i];
    return 48000;
}

/* kbit/s for each channel: 64 by default, 16 to 160 by quality. */
static uint32_t channel_kbps(const struct job *j)
{
    return j->quality ? 16 + j->quality * 144 / 100 : 64;
}

static void sound_out(struct args *a, const struct job *j, int wma)
{
    uint32_t rate = coded_rate(j->rate), kbps = channel_kbps(j) * j->channels;
    if (wma) {
        /* WMA 2 takes at most about 3 bits a sample at a rate: kept under it */
        uint32_t most = rate * j->channels * 3 / 1000;
        if (kbps > most)
            kbps = most;
    }
    arg(a, "-c:a"); arg(a, "%s", wma ? "wmav2" : "aac");
    arg(a, "-b:a"); arg(a, "%uk", kbps);
    arg(a, "-ar"); arg(a, "%u", rate);
}

/* x265's constant rate factor: 51 (worst) to 0 by quality; 23 by default, 20 for pictures. */
static uint32_t crf(const struct job *j)
{
    if (!j->quality)
        return j->kind == K_PICTURE ? 20 : 23;
    return 51 - j->quality * 51 / 100;
}

/* An odd side is padded to even (4:2:0 needs it) with the last row or column repeated. */
static void even_pad(char *out, size_t room, const struct job *j, const char *then)
{
    uint32_t cw = (j->width + 1) & ~1u, ch = (j->height + 1) & ~1u;
    if (cw == j->width && ch == j->height)
        snprintf(out, room, "%s", then);
    else
        snprintf(out, room, "pad=%u:%u:0:0,fillborders=right=%u:bottom=%u:mode=smear%s%s", cw, ch, cw - j->width,
                 ch - j->height, *then ? "," : "", then);
}

static int run(struct args *a)
{
    return hr_run(a->v, -1) ? MD_HOSTERROR : MD_OK;
}

static int finish_picture(struct job *j, const char *out)
{
    struct args a;
    struct heif_picture p;
    char colour[200], filter[600], path[300], apath[300];
    uint8_t *hevc = NULL, *alpha = NULL, *file = NULL;
    size_t hn = 0, an = 0, fn = 0;
    int st = MD_HOSTERROR, with_alpha = j->pixels == ME_PIX_ARGB && (j->flags & ME_F_ALPHA);

    snprintf(path, sizeof path, "%s/picture.hevc", j->dir);
    snprintf(apath, sizeof apath, "%s/alpha.hevc", j->dir);
    start(&a);
    pixels_in(&a, j);
    /* Full-range BT.601 YCbCr, as JPEG has it; said in the VUI and in colr. */
    even_pad(colour, sizeof colour, j, "scale=out_color_matrix=bt601:out_range=full,format=yuv420p");
    if (with_alpha) {
        char al[200];
        even_pad(al, sizeof al, j, "format=gray");
        snprintf(filter, sizeof filter, "[0:v]split[c][a];[c]%s[cy];[a]alphaextract,%s[ay]", colour, al);
    } else
        snprintf(filter, sizeof filter, "[0:v]%s[cy]", colour);
    arg(&a, "-filter_complex"); arg(&a, "%s", filter);
    for (int k = 0; k < (with_alpha ? 2 : 1); k++) {
        arg(&a, "-map"); arg(&a, k ? "[ay]" : "[cy]");
        arg(&a, "-frames:v"); arg(&a, "1");
        arg(&a, "-c:v"); arg(&a, "libx265");
        arg(&a, "-preset"); arg(&a, "medium");
        arg(&a, "-crf"); arg(&a, "%u", crf(j));
        arg(&a, "-x265-params"); arg(&a, "log-level=error:keyint=1:info=0");
        arg(&a, "-color_range"); arg(&a, "pc");
        if (!k) {
            arg(&a, "-colorspace"); arg(&a, "bt470bg");
            arg(&a, "-color_primaries"); arg(&a, "bt709");
            arg(&a, "-color_trc"); arg(&a, "iec61966-2-1");
        }
        arg(&a, "-f"); arg(&a, "hevc");
        arg(&a, "%s", k ? apath : path);
    }
    if (run(&a) != MD_OK || !(hevc = hr_read(path, &hn)) || (with_alpha && !(alpha = hr_read(apath, &an))))
        goto out;
    memset(&p, 0, sizeof p);
    p.hevc = hevc;
    p.hevc_len = hn;
    p.alpha = alpha;
    p.alpha_len = an;
    p.width = j->width;
    p.height = j->height;
    p.coded_width = (j->width + 1) & ~1u;
    p.coded_height = (j->height + 1) & ~1u;
    p.full_range = 1;
    if (heif_write(&p, &file, &fn) || hr_write(out, file, (uint32_t)fn))
        goto out;
    j->frames = 1;
    st = MD_OK;
out:
    free(hevc);
    free(alpha);
    free(file);
    return st;
}

static int finish_sound(struct job *j, const char *out)
{
    struct args a;
    start(&a);
    sound_in(&a, j);
    sound_out(&a, j, j->format == ME_FORMAT_WMA);
    arg(&a, "-f");
    if (j->format == ME_FORMAT_M4A) {
        arg(&a, "ipod");
        arg(&a, "-movflags"); arg(&a, "+faststart");
    } else
        arg(&a, j->format == ME_FORMAT_AAC ? "adts" : "asf");
    arg(&a, "%s", out);
    return run(&a);
}

static int finish_film(struct job *j, const char *out)
{
    struct args a;
    char filter[300];
    int wmv = j->format == ME_FORMAT_WMV;

    start(&a);
    pixels_in(&a, j);
    if (j->bytes[ME_STREAM_SOUND])
        sound_in(&a, j);
    even_pad(filter, sizeof filter, j, "format=yuv420p");
    arg(&a, "-vf"); arg(&a, "%s", filter);
    arg(&a, "-map"); arg(&a, "0:v");
    if (j->bytes[ME_STREAM_SOUND]) {
        arg(&a, "-map"); arg(&a, "1:a");
    }
    if (wmv) {
        arg(&a, "-c:v"); arg(&a, "wmv2");
        /* qscale 2 (best) to 31; 5 by default */
        arg(&a, "-q:v"); arg(&a, "%u", j->quality ? 31 - j->quality * 29 / 100 : 5);
    } else {
        arg(&a, "-c:v"); arg(&a, "libx265");
        arg(&a, "-preset"); arg(&a, "fast");
        arg(&a, "-crf"); arg(&a, "%u", crf(j));
        arg(&a, "-x265-params"); arg(&a, "log-level=error:info=0");
        arg(&a, "-tag:v"); arg(&a, "hvc1");
    }
    if (j->bytes[ME_STREAM_SOUND])
        sound_out(&a, j, wmv);
    arg(&a, "-f");
    if (wmv)
        arg(&a, "asf");
    else {
        arg(&a, "mp4");
        arg(&a, "-movflags"); arg(&a, "+faststart");
    }
    arg(&a, "%s", out);
    return run(&a);
}

/* FFmpeg's work, on a thread of its own, so no call waits on it for long. */
static void *work(void *arg)
{
    struct job *j = arg;
    char out[300];
    struct stat sb;
    int st;

    snprintf(out, sizeof out, "%s/out", j->dir);
    st = j->kind == K_PICTURE ? finish_picture(j, out) : j->kind == K_SOUND ? finish_sound(j, out)
                                                                          : finish_film(j, out);
    if (st == MD_OK && (stat(out, &sb) || sb.st_size <= 0 || (uint64_t)sb.st_size > 0xffffffffu))
        st = MD_HOSTERROR;
    /* the Amiga's bytes are not needed again */
    snprintf(out, sizeof out, "%s/pixels.raw", j->dir);
    unlink(out);
    snprintf(out, sizeof out, "%s/sound.raw", j->dir);
    unlink(out);
    pthread_mutex_lock(&lock);
    j->status = st;
    if (st == MD_OK) {
        j->size = (uint64_t)sb.st_size;
        j->finished = 1;
    }
    j->running = 0;
    if (j->closed)
        job_free(j);
    pthread_cond_broadcast(&done);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* How long FINISH waits for FFmpeg before it answers "still working": well
 * inside a LAN request's 60 seconds. OPENSERVICE_ENCODE_WAIT in ms. */
static void deadline(struct timespec *ts)
{
    const char *e = getenv("OPENSERVICE_ENCODE_WAIT");
    long ms = e && *e ? atol(e) : 20000;
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int finish(struct job *j, struct md_buffer *info, uint32_t *result, uint32_t *aux)
{
    struct timespec until;
    int st;

    if (!j->started) {
        pthread_t t;
        pthread_attr_t at;
        for (int s = 0; s < 2; s++)
            if (j->stream[s]) {
                int bad = fclose(j->stream[s]) != 0;
                j->stream[s] = NULL;
                if (bad)
                    return MD_HOSTERROR;
            }
        /* whole pictures, frames and sample frames, at least one */
        if (j->kind != K_SOUND) {
            uint64_t fb = frame_bytes(j);
            if (!j->bytes[0] || j->bytes[0] % fb || (j->kind == K_PICTURE && j->bytes[0] != fb))
                return MD_BADREQUEST;
            j->frames = (uint32_t)(j->bytes[0] / fb);
        }
        if (j->rate) {
            if (j->bytes[1] % sample_bytes(j) || (j->kind == K_SOUND && !j->bytes[1]))
                return MD_BADREQUEST;
            j->sample_frames = (uint32_t)(j->bytes[1] / sample_bytes(j));
        }
        pthread_mutex_lock(&lock);
        j->started = j->running = 1;
        pthread_mutex_unlock(&lock);
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, work, j)) {
            pthread_mutex_lock(&lock);
            j->running = 0;
            j->status = MD_HOSTERROR;
            pthread_mutex_unlock(&lock);
        }
        pthread_attr_destroy(&at);
    }
    deadline(&until);
    pthread_mutex_lock(&lock);
    while (j->running && pthread_cond_timedwait(&done, &lock, &until) == 0)
        ;
    if (j->running) {                                     /* not yet: FINISH again */
        pthread_mutex_unlock(&lock);
        *aux = 1;
        return MD_OK;
    }
    st = j->status;
    pthread_mutex_unlock(&lock);
    if (st != MD_OK)
        return st;
    if (info->out) {
        if (info->length < ME_INFO_SIZE)
            return MD_TOOSMALL;
        put32(info->out, j->format);
        put32(info->out + 4, (uint32_t)j->size);
        put32(info->out + 8, j->kind == K_SOUND ? 0 : j->width);
        put32(info->out + 12, j->kind == K_SOUND ? 0 : j->height);
        put32(info->out + 16, j->frames);
        put32(info->out + 20, j->sample_frames);
        info->written = ME_INFO_SIZE;
    }
    *result = (uint32_t)j->size;
    return MD_OK;
}

static int read_out(struct job *j, uint32_t offset, struct md_buffer *b, uint32_t *result)
{
    char path[300];
    ssize_t got = 0;
    int fd;

    if (!j->finished || !b->out)
        return MD_BADREQUEST;
    *result = 0;
    if (offset >= j->size || !b->length)
        return MD_OK;
    snprintf(path, sizeof path, "%s/out", j->dir);
    if ((fd = open(path, O_RDONLY)) < 0)
        return MD_HOSTERROR;
    while ((uint32_t)got < b->length) {
        ssize_t n = pread(fd, b->out + got, b->length - got, (off_t)offset + got);
        if (n < 0) {
            close(fd);
            return MD_HOSTERROR;
        }
        if (!n)
            break;
        got += n;
    }
    close(fd);
    b->written = (uint32_t)got;
    *result = (uint32_t)got;
    return MD_OK;
}

int me_call(uint16_t op, uint32_t arg_, const uint32_t extra[4], struct md_buffer buf[4],
            uint32_t *result, uint32_t *aux)
{
    struct job *j;
    int st;

    *result = *aux = 0;
    switch (op) {
    case ME_FORMATS: {
        uint32_t n = 0;
        pthread_mutex_lock(&lock);
        find_formats();
        for (uint32_t i = 0; i < offered.count; i++)
            if (buf[1].out && (i + 1) * 4 <= buf[1].length) {
                put32(buf[1].out + i * 4, offered.list[i]);
                n++;
            }
        st = n < offered.count && buf[1].out ? MD_TOOSMALL : MD_OK;
        if (buf[1].out)
            buf[1].written = n * 4;
        *result = offered.count;
        pthread_mutex_unlock(&lock);
        return st;
    }
    case ME_BEGIN:
        return begin(&buf[0], result);
    case ME_CLOSE:
        pthread_mutex_lock(&lock);
        j = find(arg_);
        st = j && !j->busy ? MD_OK : MD_BADREQUEST;
        if (st == MD_OK && j->running) {                  /* its thread frees it when FFmpeg is done */
            j->closed = 1;
            j->id = 0;
        } else if (st == MD_OK)
            job_free(j);
        pthread_mutex_unlock(&lock);
        return st;
    case ME_PUT: case ME_FINISH: case ME_READ:
        if (!(j = take(arg_)))
            return MD_BADREQUEST;
        st = op == ME_PUT ? put_bytes(j, extra[0], &buf[0], result)
           : op == ME_FINISH ? finish(j, &buf[1], result, aux)
           : read_out(j, extra[0], &buf[1], result);
        give(j);
        return st;
    default:
        return MD_BADREQUEST;
    }
}
