/*
 * media-handler: MEDIA:, a read-only DOS handler on top of
 * gfopenservice.device (docs/MEDIA_HANDLER.md). Opening
 * MEDIA:<kind>[,<w>x<h>]/<path> reads the file at <path>, has
 * media.decode/1 decode it, and gives it back as an ILBM, 8SVX or AIFF, or
 * as raw pixels or samples.
 *
 * One process does everything, a packet at a time. Packets arrive on a port
 * of the handler's own (dn_Task), so the DOS calls it makes to read the
 * sources keep the process's port for their replies.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <string.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "devices/mitchy.h"

const char HandlerVersion[] __attribute__((used)) = "$VER: media-handler 1.0 (5.11.2026)";

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

/* media.decode/1 (docs/MEDIA_DECODE.md) */
#define MD_PROBE  1
#define MD_DECODE 2
#define MD_VOPEN  3
#define MD_VFRAME 4
#define MD_VCLOSE 5
#define MD_KIND_PICTURE 1
#define MD_KIND_SOUND   3

#define PIECE (1024UL * 1024)   /* the most 16-bit sound one DECODE gives */
#define SND_RATE_8SVX 28000

enum { K_ILBM, K_ILBM8, K_8SVX, K_AIFF, K_RAW, NKINDS };
static const char *const kindNames[NKINDS] = { "ilbm", "ilbm8", "8svx", "aiff", "raw" };

enum { O_ROOT, O_KIND, O_FILE, O_SDIR };   /* what a lock stands for: O_SDIR a source's directory */

struct object {
    UBYTE type, kind;
    UWORD maxw, maxh;
    char path[256];                  /* O_FILE, O_SDIR: the source */
};

struct mlock {
    struct FileLock fl;              /* first: the BPTR DOS holds */
    struct object o;
    BPTR src;                        /* O_SDIR: the directory being listed */
    struct FileInfoBlock *fib;
};

enum { P_PICTURE, P_VIDEO, P_SOUND };

/* What a source turns into, from PROBE (or VOPEN). */
struct plan {
    UBYTE what;
    ULONG w, h;                      /* the pixels written */
    ULONG frames, rate, channels;    /* a sound's, as DECODE gives them */
    ULONG head, data, size;          /* the converted file's header, data and whole size */
};

struct mfile {
    struct object o;
    struct plan p;
    struct DateStamp date;
    ULONG pos;
    UBYTE *out;                      /* pictures: the whole converted file */
    UBYTE *src;                      /* sounds: the source, for DECODE */
    ULONG srcLen;
    UBYTE head[64];                  /* sounds: the header */
    UBYTE *piece;                    /* sounds: what DECODE gave last, converted */
    ULONG pieceFirst, pieceFrames;
};

static struct MsgPort *port;         /* dn_Task: packets arrive here */
static struct DeviceNode *devNode;
static char devName[32];
static LONG locks, files;

/* ---- the service ----------------------------------------------------------- */

static struct MsgPort *ioPort;
static struct OSRequest *io;
static UWORD service;
static int haveService;

static void serviceClose(void)
{
    if (haveService) {
        io->os_Service = service;
        io->os_Req.io_Command = OSCMD_CLOSE;
        DoIO((struct IORequest *)io);
        haveService = 0;
    }
    if (io && io->os_Req.io_Device) {
        CloseDevice((struct IORequest *)io);
        io->os_Req.io_Device = NULL;
    }
}

/* Opened when first needed, and again after the card or Cradle went away. */
static LONG serviceOpen(void)
{
    if (haveService)
        return 0;
    if (!ioPort && !(ioPort = CreateMsgPort()))
        return ERROR_NO_FREE_STORE;
    if (!io && !(io = (struct OSRequest *)CreateIORequest(ioPort, sizeof *io)))
        return ERROR_NO_FREE_STORE;
    if (OpenDevice((CONST_STRPTR)MITCHY_NAME, 0, (struct IORequest *)io, 0)) {
        io->os_Req.io_Device = NULL;
        return ERROR_NOT_IMPLEMENTED;
    }
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = (APTR)"media.decode/1";
    io->os_Buf[0].ob_Length = 14;
    io->os_Req.io_Command = OSCMD_OPEN;
    DoIO((struct IORequest *)io);
    if (io->os_Req.io_Error || io->os_Status) {
        serviceClose();
        return ERROR_NOT_IMPLEMENTED;
    }
    service = (UWORD)io->os_Result;
    haveService = 1;
    return 0;
}

static LONG call(UWORD op, ULONG arg, const ULONG extra[4], APTR in, ULONG inLen, APTR out, ULONG outLen,
                 ULONG *result, ULONG *aux)
{
    LONG e = serviceOpen();
    if (e)
        return e;
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Req.io_Command = OSCMD_CALL;
    io->os_Service = service;
    io->os_Op = op;
    io->os_Flags = out ? 2 : 0;
    io->os_Arg = arg;
    io->os_Buf[0].ob_Data = in;
    io->os_Buf[0].ob_Length = in ? inLen : 0;
    io->os_Buf[1].ob_Data = out;
    io->os_Buf[1].ob_Length = out ? outLen : 0;
    memcpy(io->os_Extra, extra, sizeof io->os_Extra);
    DoIO((struct IORequest *)io);
    if (io->os_Req.io_Error || io->os_Status == OSERR_NOSERVICE || io->os_Status == OSERR_LOST) {
        serviceClose();
        return ERROR_NOT_IMPLEMENTED;
    }
    if (io->os_Status)
        return ERROR_OBJECT_WRONG_TYPE;
    if (result)
        *result = io->os_Result;
    if (aux)
        *aux = io->os_Aux;
    return 0;
}

/* ---- small things ---------------------------------------------------------- */

static ULONG get32(const UBYTE *p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

static UBYTE *put32(UBYTE *p, ULONG v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
    return p + 4;
}

static UBYTE *put16(UBYTE *p, ULONG v)
{
    p[0] = v >> 8;
    p[1] = v;
    return p + 2;
}

static UBYTE *putId(UBYTE *p, const char *id)
{
    memcpy(p, id, 4);
    return p + 4;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static void toBstr(UBYTE *b, const char *s, ULONG room)
{
    ULONG n = strlen(s);
    if (n > room - 1)
        n = room - 1;
    b[0] = (UBYTE)n;
    memcpy(b + 1, s, n);
}

/* The extension as media.decode/1's hint: up to four letters, capitals, space-padded. */
static ULONG hintOf(const char *path)
{
    const char *dot = strrchr((const char *)FilePart((CONST_STRPTR)path), '.');
    ULONG h = 0;
    int i;
    if (!dot || !dot[1] || strlen(dot + 1) > 4)
        return 0;
    for (i = 0; i < 4; i++) {
        int c = dot[1 + i];
        h = h << 8 | (c ? (c >= 'a' && c <= 'z' ? c - 32 : c) : ' ');
        if (!c)
            for (i++; i < 4; i++)
                h = h << 8 | ' ';
    }
    return h;
}

/* ---- the sources ----------------------------------------------------------- */

/* The source's size and date, and that it is not on MEDIA: itself. A
 * directory is taken only when dir is given, and says so there. */
static LONG sourceInfo(const char *path, ULONG *len, struct DateStamp *date, BPTR *keep, int *dir)
{
    struct DevProc *dvp;
    struct FileInfoBlock *fib;
    BPTR lock;
    LONG e = 0;

    if (!strchr(path, ':'))
        return ERROR_OBJECT_NOT_FOUND;
    if (!(dvp = GetDeviceProc((CONST_STRPTR)path, NULL)))
        return IoErr() ? IoErr() : ERROR_OBJECT_NOT_FOUND;
    if (dvp->dvp_Port == port)
        e = ERROR_INVALID_COMPONENT_NAME;    /* it would wait for itself */
    FreeDeviceProc(dvp);
    if (e)
        return e;
    if (!(lock = Lock((CONST_STRPTR)path, SHARED_LOCK)))
        return IoErr();
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) {
        UnLock(lock);
        return ERROR_NO_FREE_STORE;
    }
    if (!Examine(lock, fib))
        e = IoErr();
    else if (fib->fib_DirEntryType >= 0 && !dir)
        e = ERROR_OBJECT_WRONG_TYPE;
    else {
        if (dir)
            *dir = fib->fib_DirEntryType >= 0;
        *len = (ULONG)fib->fib_Size;
        *date = fib->fib_Date;
    }
    FreeDosObject(DOS_FIB, fib);
    if (e || !keep)
        UnLock(lock);
    else
        *keep = lock;
    return e;
}

/* The source, whole, in memory. */
static LONG sourceLoad(const char *path, UBYTE **data, ULONG *len, struct DateStamp *date)
{
    BPTR lock = 0, fh;
    LONG e = sourceInfo(path, len, date, &lock, NULL);
    if (e)
        return e;
    if (!*len) {
        UnLock(lock);
        return ERROR_OBJECT_WRONG_TYPE;
    }
    if (!(fh = OpenFromLock(lock))) {
        e = IoErr();
        UnLock(lock);
        return e;
    }
    if (!(*data = AllocVec(*len, MEMF_ANY)))
        e = ERROR_NO_FREE_STORE;
    else if (Read(fh, *data, *len) != (LONG)*len) {
        e = IoErr() ? IoErr() : ERROR_SEEK_ERROR;
        FreeVec(*data);
        *data = NULL;
    }
    Close(fh);
    return e;
}

/* ---- plans ----------------------------------------------------------------- */

static ULONG rowBytes(ULONG w)
{
    return ((w + 15) >> 4) << 1;
}

static void sizes(const struct object *o, struct plan *p)
{
    switch (o->kind) {
    case K_ILBM:
        p->head = 12 + 28 + 8;
        p->data = rowBytes(p->w) * 24 * p->h;
        break;
    case K_ILBM8:
        p->head = 12 + 28 + 8 + 768 + 8;
        p->data = rowBytes(p->w) * 8 * p->h;
        break;
    case K_8SVX:
        p->head = 12 + 28 + 8;
        p->data = p->frames;
        break;
    case K_AIFF:
        p->head = 12 + 26 + 16;
        p->data = p->frames * p->channels * 2;
        break;
    default:
        p->head = 20;
        p->data = p->what == P_SOUND ? p->frames * p->channels * 2 : p->w * p->h * 4;
    }
    p->size = p->head + p->data + (p->data & 1);
}

static LONG soundPlan(const struct object *o, UBYTE *src, ULONG len, ULONG ex[4], struct plan *p)
{
    UBYTE info[24];
    LONG e = call(MD_PROBE, 0, ex, src, len, info, sizeof info, NULL, NULL);
    if (e)
        return e;
    if (get32(info) != MD_KIND_SOUND || !get32(info + 16))
        return ERROR_OBJECT_WRONG_TYPE;
    p->what = P_SOUND;
    p->frames = get32(info + 12);
    p->rate = get32(info + 16);
    p->channels = get32(info + 20);
    if (p->channels < 1 || p->channels > ex[0])
        return ERROR_OBJECT_WRONG_TYPE;
    return 0;
}

/* PROBE, or VOPEN for a video; *video keeps the video open for its frame. */
static LONG makePlan(const struct object *o, UBYTE *src, ULONG len, struct plan *p, ULONG *video)
{
    UBYTE info[24];
    ULONG ex[4], handle;
    LONG e, ev;

    memset(p, 0, sizeof *p);
    ex[2] = hintOf(o->path);
    ex[3] = 0;
    if (o->kind == K_8SVX || o->kind == K_AIFF) {
        ex[0] = o->kind == K_8SVX ? 1 : 2;
        ex[1] = o->kind == K_8SVX ? SND_RATE_8SVX : 0;
        if ((e = soundPlan(o, src, len, ex, p)))
            return e;
    } else {
        ex[0] = o->maxw;
        ex[1] = o->maxh;
        e = call(MD_PROBE, 0, ex, src, len, info, sizeof info, NULL, NULL);
        if (e == ERROR_NOT_IMPLEMENTED)
            return e;
        if (!e && get32(info) == MD_KIND_PICTURE) {
            p->what = P_PICTURE;
            p->w = get32(info + 16);
            p->h = get32(info + 20);
        } else {
            /* Not a picture: a video's first frame, or for raw a sound. */
            ex[2] = 0;
            ev = call(MD_VOPEN, 0, ex, src, len, info, sizeof info, &handle, NULL);
            if (!ev) {
                p->what = P_VIDEO;
                p->w = get32(info + 16);
                p->h = get32(info + 20);
                if (video)
                    *video = handle;
                else {
                    ex[0] = ex[1] = ex[2] = 0;
                    call(MD_VCLOSE, handle, ex, NULL, 0, NULL, 0, NULL, NULL);
                }
            } else if (ev == ERROR_NOT_IMPLEMENTED)
                return ev;
            else if (o->kind == K_RAW && !e && get32(info) == MD_KIND_SOUND) {
                ex[0] = 2;
                ex[1] = 0;
                ex[2] = hintOf(o->path);
                if ((e = soundPlan(o, src, len, ex, p)))
                    return e;
            } else
                return ERROR_OBJECT_WRONG_TYPE;
        }
        if (p->what != P_SOUND && (!p->w || !p->h || p->w > 32767 || p->h > 32767))
            return ERROR_OBJECT_WRONG_TYPE;
    }
    sizes(o, p);
    return 0;
}

/* The last plan made, so Lock, Examine and Open probe once. */
static struct {
    int valid;
    struct object o;
    struct DateStamp date;
    ULONG len;
    struct plan p;
} cache;

static int cached(const struct object *o, ULONG len, const struct DateStamp *date)
{
    return cache.valid && cache.o.kind == o->kind && cache.o.maxw == o->maxw && cache.o.maxh == o->maxh
           && !strcmp(cache.o.path, o->path) && cache.len == len && !CompareDates(&cache.date, date);
}

static void remember(const struct object *o, ULONG len, const struct DateStamp *date, const struct plan *p)
{
    cache.o = *o;
    cache.len = len;
    cache.date = *date;
    cache.p = *p;
    cache.valid = 1;
}

/* For Examine: the converted size without decoding; 0 when it is not known. */
static LONG fileSize(const struct object *o, ULONG *size, struct DateStamp *date)
{
    UBYTE *src = NULL;
    ULONG len;
    struct plan p;
    LONG e = sourceInfo(o->path, &len, date, NULL, NULL);
    *size = 0;
    if (e)
        return e;
    if (!cached(o, len, date)) {
        if (sourceLoad(o->path, &src, &len, date))
            return 0;
        e = makePlan(o, src, len, &p, NULL);
        FreeVec(src);
        if (e)
            return 0;
        remember(o, len, date, &p);
    }
    *size = cache.p.size;
    return 0;
}

/* ---- converting ------------------------------------------------------------ */

/* One row of 8 bitplanes from 8-bit values v[x * step]: bit b into plane b. */
static void planes(UBYTE *dst, ULONG rb, const UBYTE *v, ULONG step, ULONG w)
{
    ULONG x;
    int b, k, n;
    UBYTE s[8], o;
    for (x = 0; x < w; x += 8) {
        n = w - x < 8 ? (int)(w - x) : 8;
        for (k = 0; k < 8; k++)
            s[k] = k < n ? v[(x + k) * step] : 0;
        for (b = 0; b < 8; b++) {
            o = 0;
            for (k = 0; k < 8; k++)
                o = (UBYTE)(o << 1 | ((s[k] >> b) & 1));
            dst[b * rb + (x >> 3)] = o;
        }
    }
}

/* The 6 x 6 x 6 cube and grey ramp of media.decode/1's 256-colour frames. */
static void cubeColour(int i, UBYTE *rgb)
{
    if (i < 216) {
        rgb[0] = (UBYTE)(i / 36 * 51);
        rgb[1] = (UBYTE)(i / 6 % 6 * 51);
        rgb[2] = (UBYTE)(i % 6 * 51);
    } else
        rgb[0] = rgb[1] = rgb[2] = (UBYTE)((i - 216) * 255 / 39);
}

static const UBYTE bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* An ARGB row into cube indices, 4 x 4 ordered dither. */
static void dither(UBYTE *dst, const UBYTE *argb, ULONG w, ULONG y)
{
    ULONG x, t;
    for (x = 0; x < w; x++, argb += 4) {
        t = bayer[y & 3][x & 3] * 16 + 8;
        dst[x] = (UBYTE)(((argb[1] * 5 + t) >> 8) * 36 + ((argb[2] * 5 + t) >> 8) * 6 + ((argb[3] * 5 + t) >> 8));
    }
}

static UBYTE *ilbmHead(UBYTE *q, const struct plan *p, int depth)
{
    int i;
    q = putId(q, "FORM");
    q = put32(q, p->size - 8);
    q = putId(q, "ILBM");
    q = putId(q, "BMHD");
    q = put32(q, 20);
    q = put16(q, p->w);
    q = put16(q, p->h);
    q = put32(q, 0);                 /* x, y */
    *q++ = (UBYTE)depth;
    *q++ = 0;                        /* no mask */
    *q++ = 0;                        /* not compressed: the size is known before decoding */
    *q++ = 0;
    q = put16(q, 0);                 /* transparent colour */
    *q++ = 1;                        /* square pixels */
    *q++ = 1;
    q = put16(q, p->w);
    q = put16(q, p->h);
    if (depth == 8) {
        q = putId(q, "CMAP");
        q = put32(q, 768);
        for (i = 0; i < 256; i++, q += 3)
            cubeColour(i, q);
    }
    q = putId(q, "BODY");
    return put32(q, p->data);
}

/* px: ARGB (bpp 4), RGB (3) or cube indices (1). */
static void toIlbm(UBYTE *out, const struct plan *p, int depth, const UBYTE *px, int bpp, UBYTE *row)
{
    ULONG rb = rowBytes(p->w), y;
    UBYTE *q = ilbmHead(out, p, depth);
    int c, first = bpp == 4;
    for (y = 0; y < p->h; y++, px += p->w * bpp) {
        if (depth == 24)
            for (c = 0; c < 3; c++, q += rb * 8)
                planes(q, rb, px + first + c, bpp, p->w);
        else {
            if (bpp == 4)
                dither(row, px, p->w, y);
            planes(q, rb, bpp == 4 ? row : px, 1, p->w);
            q += rb * 8;
        }
    }
}

static void soundHead(UBYTE *q, const struct object *o, const struct plan *p)
{
    ULONG r = p->rate;
    int n = 31;
    q = putId(q, o->kind == K_RAW ? "MRAW" : "FORM");
    if (o->kind == K_RAW) {
        q = putId(q, "S16B");
        q = put32(q, p->rate);
        q = put32(q, p->channels);
        put32(q, p->data);
        return;
    }
    q = put32(q, p->size - 8);
    if (o->kind == K_8SVX) {
        q = putId(q, "8SVX");
        q = putId(q, "VHDR");
        q = put32(q, 20);
        q = put32(q, p->frames);     /* one-shot */
        q = put32(q, 0);
        q = put32(q, 0);
        q = put16(q, p->rate);
        *q++ = 1;                    /* one octave */
        *q++ = 0;                    /* not compressed */
        q = put32(q, 0x10000);       /* full volume */
        q = putId(q, "BODY");
        put32(q, p->data);
        return;
    }
    q = putId(q, "AIFF");
    q = putId(q, "COMM");
    q = put32(q, 18);
    q = put16(q, p->channels);
    q = put32(q, p->frames);
    q = put16(q, 16);
    /* The rate as an 80-bit extended float. */
    while (r && !(r & 0x80000000UL)) {
        r <<= 1;
        n--;
    }
    q = put16(q, r ? 16383 + n : 0);
    q = put32(q, r);
    q = put32(q, 0);
    q = putId(q, "SSND");
    q = put32(q, 8 + p->data);
    q = put32(q, 0);
    put32(q, 0);
}

static void rawHead(UBYTE *q, const struct plan *p)
{
    q = putId(q, "MRAW");
    q = putId(q, "ARGB");
    q = put32(q, p->w);
    q = put32(q, p->h);
    put32(q, p->data);
}

/* A picture or a video's first frame, whole, into f->out. */
static LONG convertPicture(struct mfile *f, UBYTE *src, ULONG len, ULONG video)
{
    struct plan *p = &f->p;
    ULONG ex[4], r, a, n = p->w * p->h, i;
    int bpp = p->what == P_PICTURE ? 4 : f->o.kind == K_ILBM8 ? 1 : 3;
    UBYTE *px, *row = NULL;
    LONG e;

    if (!(px = AllocVec(n * bpp, MEMF_ANY)) || !(f->out = AllocVec(p->size, MEMF_ANY | MEMF_CLEAR))
        || !(row = AllocVec(p->w + 16, MEMF_ANY))) {
        e = ERROR_NO_FREE_STORE;
        goto done;
    }
    if (p->what == P_PICTURE) {
        ex[0] = f->o.maxw;
        ex[1] = f->o.maxh;
        ex[2] = hintOf(f->o.path);
        ex[3] = 0;
        e = call(MD_DECODE, 0, ex, src, len, px, n * 4, &r, &a);
        if (!e && (r != p->w || a != p->h))
            e = ERROR_OBJECT_WRONG_TYPE;
    } else {
        ex[0] = 0;
        ex[1] = bpp == 1 ? 0 : 1;
        ex[2] = ex[3] = 0;
        e = call(MD_VFRAME, video, ex, NULL, 0, px, n * bpp, &r, NULL);
    }
    if (e)
        goto done;
    if (f->o.kind == K_RAW) {
        rawHead(f->out, p);
        for (i = 0; i < n; i++) {
            UBYTE *q = f->out + p->head + i * 4;
            if (bpp == 4)
                memcpy(q, px + i * 4, 4);
            else {
                q[0] = 255;
                memcpy(q + 1, px + i * 3, 3);
            }
        }
    } else
        toIlbm(f->out, p, f->o.kind == K_ILBM8 ? 8 : 24, px, bpp, row);
done:
    if (row)
        FreeVec(row);
    if (px)
        FreeVec(px);
    return e;
}

/* Sounds are decoded a piece at a time, as they are read. */
static LONG soundPiece(struct mfile *f, ULONG frame)
{
    struct plan *p = &f->p;
    ULONG bpf = p->channels * 2, want = PIECE / bpf, got = 0, ex[4], i;
    LONG e;

    if (want > p->frames - frame)
        want = p->frames - frame;
    ex[0] = f->o.kind == K_8SVX ? 1 : 2;
    ex[1] = f->o.kind == K_8SVX ? SND_RATE_8SVX : 0;
    ex[2] = hintOf(f->o.path);
    ex[3] = 0;
    if ((e = call(MD_DECODE, frame, ex, f->src, f->srcLen, f->piece, want * bpf, &got, NULL)))
        return e;
    if (got > want)
        got = want;
    memset(f->piece + got * bpf, 0, (want - got) * bpf);   /* shorter than PROBE said: silence */
    if (f->o.kind == K_8SVX)
        for (i = 0; i < want; i++)
            f->piece[i] = f->piece[i * 2];
    f->pieceFirst = frame;
    f->pieceFrames = want;
    return 0;
}

static void fileFree(struct mfile *f)
{
    if (f->out)
        FreeVec(f->out);
    if (f->src)
        FreeVec(f->src);
    if (f->piece)
        FreeVec(f->piece);
    FreeVec(f);
}

static struct mfile *fileOpen(const struct object *o, LONG *err)
{
    struct mfile *f;
    UBYTE *src = NULL;
    ULONG len, video = 0;
    LONG e;

    if (!(f = AllocVec(sizeof *f, MEMF_ANY | MEMF_CLEAR))) {
        *err = ERROR_NO_FREE_STORE;
        return NULL;
    }
    f->o = *o;
    if ((e = sourceLoad(o->path, &src, &len, &f->date)))
        goto fail;
    if (cached(o, len, &f->date) && cache.p.what != P_VIDEO)
        f->p = cache.p;
    else {
        if ((e = makePlan(o, src, len, &f->p, &video))) {
            if (video) {
                ULONG ex[4] = { 0, 0, 0, 0 };
                call(MD_VCLOSE, video, ex, NULL, 0, NULL, 0, NULL, NULL);
            }
            goto fail;
        }
        remember(o, len, &f->date, &f->p);
    }
    if (f->p.what == P_SOUND) {
        if (!(f->piece = AllocVec(PIECE, MEMF_ANY))) {
            e = ERROR_NO_FREE_STORE;
            goto fail;
        }
        soundHead(f->head, o, &f->p);
        f->src = src;
        f->srcLen = len;
    } else {
        e = convertPicture(f, src, len, video);
        FreeVec(src);
        src = NULL;
        if (video) {
            ULONG ex[4] = { 0, 0, 0, 0 };
            call(MD_VCLOSE, video, ex, NULL, 0, NULL, 0, NULL, NULL);
        }
        if (e)
            goto fail;
    }
    files++;
    return f;
fail:
    if (src && src != f->src)
        FreeVec(src);
    fileFree(f);
    *err = e;
    return NULL;
}

static LONG fileRead(struct mfile *f, UBYTE *buf, LONG len, LONG *err)
{
    struct plan *p = &f->p;
    ULONG done = 0, n, d, bpf, frame, at;

    if (len < 0)
        len = 0;
    if ((ULONG)len > p->size - f->pos)
        len = (LONG)(p->size - f->pos);
    while (done < (ULONG)len) {
        if (f->out) {
            memcpy(buf, f->out + f->pos, len);
            done = len;
            break;
        }
        if (f->pos < p->head) {
            n = p->head - f->pos;
            if (n > len - done)
                n = len - done;
            memcpy(buf + done, f->head + f->pos, n);
        } else if (f->pos >= p->head + p->data) {
            n = 1;                   /* the pad byte */
            buf[done] = 0;
        } else {
            d = f->pos - p->head;
            bpf = f->o.kind == K_8SVX ? 1 : p->channels * 2;
            frame = d / bpf;
            if (frame < f->pieceFirst || frame >= f->pieceFirst + f->pieceFrames || !f->pieceFrames)
                if ((*err = soundPiece(f, frame)))
                    return done ? (LONG)done : -1;
            at = d - f->pieceFirst * bpf;
            n = f->pieceFrames * bpf - at;
            if (n > len - done)
                n = len - done;
            memcpy(buf + done, f->piece + at, n);
        }
        done += n;
        f->pos += n;
    }
    if (f->out)
        f->pos += done;
    return (LONG)done;
}

/* ---- names and locks ------------------------------------------------------- */

static LONG parseKind(const char *s, ULONG n, struct object *o)
{
    ULONG k, i, end = n, w = 0, h = 0;
    for (i = 0; i < n; i++)
        if (s[i] == ',') {
            end = i;
            break;
        }
    for (k = 0; k < NKINDS; k++) {
        const char *name = kindNames[k];
        for (i = 0; i < end && name[i] && lower(s[i]) == name[i]; i++)
            ;
        if (i == end && !name[i])
            break;
    }
    if (k == NKINDS)
        return ERROR_OBJECT_NOT_FOUND;
    o->kind = (UBYTE)k;
    o->maxw = o->maxh = 0;
    if (end == n)
        return 0;
    for (i = end + 1; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        w = w * 10 + (s[i] - '0');
    if (i >= n || lower(s[i]) != 'x' || i == end + 1)
        return ERROR_INVALID_COMPONENT_NAME;
    for (k = ++i; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        h = h * 10 + (s[i] - '0');
    if (i != n || i == k || !w || !h || w > 32767 || h > 32767)
        return ERROR_INVALID_COMPONENT_NAME;
    o->maxw = (UWORD)w;
    o->maxh = (UWORD)h;
    return 0;
}

static void lockObject(BPTR lock, struct object *o)
{
    if (lock)
        *o = ((struct mlock *)BADDR(lock))->o;
    else {
        memset(o, 0, sizeof *o);
        o->type = O_ROOT;
    }
}

/* A source's parent is its directory, up to its top one ("Work:Pics", named
 * so in full so NameFromLock() gives MEDIA:ilbm/Work:Pics/...); then the
 * kind. */
static void parentOf(struct object *o)
{
    char *colon, *part;
    if (o->type == O_KIND) {
        o->type = O_ROOT;
        return;
    }
    colon = strchr(o->path, ':');
    if (!colon[1] || (o->type == O_SDIR && !strchr(colon, '/'))) {
        o->type = O_KIND;
        o->path[0] = 0;
        return;
    }
    part = (char *)PathPart((STRPTR)o->path);
    if (part <= colon)
        part = colon + 1;
    *part = 0;
    o->type = O_SDIR;
}

/* The rest of a name as a source's path, after a kind or in a source's
 * directory. */
static LONG sourcePath(struct object *o, const char *s)
{
    ULONG n = 0, k = strlen(s);
    if (o->type == O_SDIR) {
        n = strlen(o->path);
        if (o->path[n - 1] != ':')
            o->path[n++] = '/';
    } else if (!strchr(s, ':'))
        return ERROR_OBJECT_NOT_FOUND;
    if (n + k >= sizeof o->path)
        return ERROR_INVALID_COMPONENT_NAME;
    memcpy(o->path + n, s, k + 1);
    n += k;
    if (n > 1 && o->path[n - 1] == '/' && o->path[n - 2] != '/' && o->path[n - 2] != ':')
        o->path[n - 1] = 0;          /* "Work:Pics/" is Work:Pics */
    o->type = O_FILE;
    return 0;
}

/* A name, from the lock it is relative to, into what it stands for. */
static LONG resolve(BPTR lock, const char *name, struct object *o)
{
    const char *s = strchr(name, ':'), *e;
    LONG err;

    lockObject(lock, o);
    s = s ? s + 1 : name;
    while (*s) {
        if (*s == '/') {
            if (o->type == O_ROOT || o->type == O_FILE)
                return ERROR_OBJECT_NOT_FOUND;
            parentOf(o);
            s++;
        } else if (o->type == O_ROOT) {
            for (e = s; *e && *e != '/'; e++)
                ;
            if ((err = parseKind(s, e - s, o)))
                return err;
            o->type = O_KIND;
            s = *e ? e + 1 : e;
        } else if (o->type == O_KIND || o->type == O_SDIR)
            return sourcePath(o, s);
        else
            return ERROR_OBJECT_NOT_FOUND;
    }
    return 0;
}

static BPTR newLock(const struct object *o, LONG access)
{
    struct mlock *l = AllocVec(sizeof *l, MEMF_PUBLIC | MEMF_CLEAR);
    if (!l)
        return 0;
    l->fl.fl_Key = o->type == O_ROOT ? 1 : 2 + o->kind;
    l->fl.fl_Access = access;
    l->fl.fl_Task = port;
    l->fl.fl_Volume = MKBADDR(devNode);
    l->o = *o;
    locks++;
    return MKBADDR(l);
}

static void freeLock(BPTR lock)
{
    struct mlock *l = BADDR(lock);
    if (l->src)
        UnLock(l->src);
    if (l->fib)
        FreeDosObject(DOS_FIB, l->fib);
    FreeVec(l);
    locks--;
}

static int sameObject(const struct object *a, const struct object *b)
{
    return a->type == b->type && (a->type == O_ROOT || (a->kind == b->kind && a->maxw == b->maxw && a->maxh == b->maxh))
           && (a->type < O_FILE || !strcmp(a->path, b->path));
}

/* A name for a source: its file part, or for a top directory the whole
 * path ("Work:", "Work:Pics"). */
static const char *sourceName(const struct object *o)
{
    const char *f = (const char *)FilePart((CONST_STRPTR)o->path);
    return o->type == O_SDIR && !strchr(o->path, '/') ? o->path : f;
}

static void kindName(const struct object *o, char *s)
{
    strcpy(s, kindNames[o->kind]);
    if (o->maxw) {
        char t[16], *q = t + sizeof t;
        ULONG v;
        int side;
        s += strlen(s);
        *s++ = ',';
        for (side = 0; side < 2; side++) {
            v = side ? o->maxh : o->maxw;
            q = t + sizeof t;
            *--q = 0;
            do
                *--q = (char)('0' + v % 10);
            while (v /= 10);
            strcpy(s, q);
            s += strlen(s);
            if (!side)
                *s++ = 'x';
        }
    }
}

static LONG examine(const struct object *o, struct FileInfoBlock *fib)
{
    char name[40];
    ULONG size = 0;
    LONG e;

    memset(fib, 0, sizeof *fib);
    if (o->type == O_ROOT) {
        fib->fib_DirEntryType = fib->fib_EntryType = ST_ROOT;
        toBstr((UBYTE *)fib->fib_FileName, devName, sizeof fib->fib_FileName);
        DateStamp(&fib->fib_Date);
    } else if (o->type == O_KIND) {
        fib->fib_DiskKey = 0;
        fib->fib_DirEntryType = fib->fib_EntryType = ST_USERDIR;
        kindName(o, name);
        toBstr((UBYTE *)fib->fib_FileName, name, sizeof fib->fib_FileName);
        DateStamp(&fib->fib_Date);
    } else if (o->type == O_SDIR) {
        ULONG len;
        int dir;
        if ((e = sourceInfo(o->path, &len, &fib->fib_Date, NULL, &dir)))
            return e;
        fib->fib_DirEntryType = fib->fib_EntryType = ST_USERDIR;
        toBstr((UBYTE *)fib->fib_FileName, sourceName(o), sizeof fib->fib_FileName);
    } else {
        if ((e = fileSize(o, &size, &fib->fib_Date)))
            return e;
        fib->fib_DirEntryType = fib->fib_EntryType = ST_FILE;
        toBstr((UBYTE *)fib->fib_FileName, sourceName(o), sizeof fib->fib_FileName);
        fib->fib_Size = (LONG)size;
        fib->fib_NumBlocks = (LONG)((size + 511) / 512);
    }
    fib->fib_Protection = FIBF_EXECUTE;     /* rw-d: copies stay deletable */
    return 0;
}

/* ExNext in a source's directory: its entries, each file's size the
 * converted one when the last plan was for it, else 0 (not known yet). */
static LONG listNext(struct mlock *l, struct FileInfoBlock *fib)
{
    struct FileInfoBlock *e;
    ULONG n;

    if (!l->fib && !(l->fib = AllocDosObject(DOS_FIB, NULL)))
        return ERROR_NO_FREE_STORE;
    if (!l->src) {
        if (!(l->src = Lock((CONST_STRPTR)l->o.path, SHARED_LOCK)))
            return IoErr();
        if (!Examine(l->src, l->fib))
            return IoErr();
    }
    if (!ExNext(l->src, l->fib))
        return IoErr();
    e = l->fib;
    memset(fib, 0, sizeof *fib);
    fib->fib_DiskKey = e->fib_DiskKey;
    fib->fib_DirEntryType = fib->fib_EntryType =
        e->fib_DirEntryType > 0 && e->fib_DirEntryType != ST_SOFTLINK ? ST_USERDIR : ST_FILE;
    toBstr((UBYTE *)fib->fib_FileName, (const char *)e->fib_FileName, sizeof fib->fib_FileName);
    fib->fib_Date = e->fib_Date;
    fib->fib_Protection = FIBF_EXECUTE;
    if (e->fib_DirEntryType < 0 && cache.valid && cache.o.kind == l->o.kind && cache.o.maxw == l->o.maxw
        && cache.o.maxh == l->o.maxh && cache.len == (ULONG)e->fib_Size && !CompareDates(&cache.date, &e->fib_Date)) {
        n = strlen(l->o.path);
        if (!strncmp(cache.o.path, l->o.path, n) && !strcmp(cache.o.path + n + (l->o.path[n - 1] != ':'), (const char *)e->fib_FileName)
            && (l->o.path[n - 1] == ':' || cache.o.path[n] == '/')) {
            fib->fib_Size = (LONG)cache.p.size;
            fib->fib_NumBlocks = (LONG)((cache.p.size + 511) / 512);
        }
    }
    return 0;
}

/* ---- packets --------------------------------------------------------------- */

static void cstr(BSTR b, char *s, ULONG room)
{
    const UBYTE *p = BADDR(b);
    ULONG n = p ? p[0] : 0;
    if (n > room - 1)
        n = room - 1;
    if (n)
        memcpy(s, p + 1, n);
    s[n] = 0;
}

static void info(struct InfoData *id)
{
    memset(id, 0, sizeof *id);
    id->id_DiskState = ID_WRITE_PROTECTED;
    id->id_BytesPerBlock = 512;
    id->id_DiskType = ID_DOS_DISK;
    id->id_InUse = locks + files ? DOSTRUE : DOSFALSE;
}

static int packet(struct DosPacket *dp)
{
    static char name[300];
    struct object o;
    struct mfile *f;
    struct FileHandle *fh;
    struct FileInfoBlock *fib;
    LONG r1 = DOSFALSE, r2 = 0, e;

    switch (dp->dp_Type) {
    case ACTION_LOCATE_OBJECT:
        cstr((BSTR)dp->dp_Arg2, name, sizeof name);
        if (!(r2 = resolve((BPTR)dp->dp_Arg1, name, &o))) {
            ULONG len;
            struct DateStamp date;
            int dir;
            if (o.type == O_FILE) {
                if ((r2 = sourceInfo(o.path, &len, &date, NULL, &dir)))
                    break;
                if (dir)
                    o.type = O_SDIR;
            }
            if (!(r1 = newLock(&o, dp->dp_Arg3)))
                r2 = ERROR_NO_FREE_STORE;
        }
        break;
    case ACTION_COPY_DIR:
        lockObject((BPTR)dp->dp_Arg1, &o);
        if (!(r1 = newLock(&o, SHARED_LOCK)))
            r2 = ERROR_NO_FREE_STORE;
        break;
    case ACTION_FREE_LOCK:
        if (dp->dp_Arg1)
            freeLock((BPTR)dp->dp_Arg1);
        r1 = DOSTRUE;
        break;
    case ACTION_PARENT:
        lockObject((BPTR)dp->dp_Arg1, &o);
        if (o.type == O_ROOT)
            break;                   /* the root has none: 0, no error */
        parentOf(&o);
        if (!(r1 = newLock(&o, SHARED_LOCK)))
            r2 = ERROR_NO_FREE_STORE;
        break;
    case ACTION_SAME_LOCK: {
        struct object b;
        lockObject((BPTR)dp->dp_Arg1, &o);
        lockObject((BPTR)dp->dp_Arg2, &b);
        r1 = sameObject(&o, &b) ? DOSTRUE : DOSFALSE;
        break;
    }
    case ACTION_EXAMINE_OBJECT:
        lockObject((BPTR)dp->dp_Arg1, &o);
        if (o.type == O_SDIR) {      /* a listing starts again */
            struct mlock *l = BADDR(dp->dp_Arg1);
            if (l->src)
                UnLock(l->src);
            l->src = 0;
        }
        if (!(r2 = examine(&o, (struct FileInfoBlock *)BADDR(dp->dp_Arg2))))
            r1 = DOSTRUE;
        break;
    case ACTION_EXAMINE_NEXT:
        lockObject((BPTR)dp->dp_Arg1, &o);
        fib = (struct FileInfoBlock *)BADDR(dp->dp_Arg2);
        if (o.type == O_SDIR) {
            if (!(r2 = listNext((struct mlock *)BADDR(dp->dp_Arg1), fib)))
                r1 = DOSTRUE;
            break;
        }
        if (o.type != O_ROOT || fib->fib_DiskKey >= NKINDS) {
            r2 = ERROR_NO_MORE_ENTRIES;
            break;
        }
        e = fib->fib_DiskKey;
        o.type = O_KIND;
        o.kind = (UBYTE)e;
        examine(&o, fib);
        fib->fib_DiskKey = e + 1;
        r1 = DOSTRUE;
        break;
    case ACTION_FINDINPUT:
    case ACTION_FH_FROM_LOCK:
        fh = (struct FileHandle *)BADDR(dp->dp_Arg1);
        if (dp->dp_Type == ACTION_FINDINPUT) {
            cstr((BSTR)dp->dp_Arg3, name, sizeof name);
            if ((r2 = resolve((BPTR)dp->dp_Arg2, name, &o)))
                break;
        } else
            lockObject((BPTR)dp->dp_Arg2, &o);
        if (o.type != O_FILE) {
            r2 = ERROR_OBJECT_WRONG_TYPE;
            break;
        }
        if (!(f = fileOpen(&o, &r2)))
            break;
        fh->fh_Arg1 = (LONG)f;
        fh->fh_Port = 0;             /* not interactive */
        if (dp->dp_Type == ACTION_FH_FROM_LOCK)
            freeLock((BPTR)dp->dp_Arg2);
        r1 = DOSTRUE;
        break;
    case ACTION_READ:
        r1 = fileRead((struct mfile *)dp->dp_Arg1, (UBYTE *)dp->dp_Arg2, dp->dp_Arg3, &r2);
        break;
    case ACTION_SEEK: {
        LONG to;
        f = (struct mfile *)dp->dp_Arg1;
        to = dp->dp_Arg3 == OFFSET_BEGINNING ? dp->dp_Arg2
             : dp->dp_Arg3 == OFFSET_CURRENT ? (LONG)f->pos + dp->dp_Arg2
             : (LONG)f->p.size + dp->dp_Arg2;
        if (to < 0 || (ULONG)to > f->p.size || dp->dp_Arg3 < OFFSET_BEGINNING || dp->dp_Arg3 > OFFSET_END) {
            r1 = -1;
            r2 = ERROR_SEEK_ERROR;
            break;
        }
        r1 = (LONG)f->pos;
        f->pos = (ULONG)to;
        break;
    }
    case ACTION_END:
        fileFree((struct mfile *)dp->dp_Arg1);
        files--;
        r1 = DOSTRUE;
        break;
    case ACTION_EXAMINE_FH:
        f = (struct mfile *)dp->dp_Arg1;
        fib = (struct FileInfoBlock *)BADDR(dp->dp_Arg2);
        memset(fib, 0, sizeof *fib);
        fib->fib_DirEntryType = fib->fib_EntryType = ST_FILE;
        toBstr((UBYTE *)fib->fib_FileName, sourceName(&f->o), sizeof fib->fib_FileName);
        fib->fib_Size = (LONG)f->p.size;
        fib->fib_NumBlocks = (LONG)((f->p.size + 511) / 512);
        fib->fib_Date = f->date;
        fib->fib_Protection = FIBF_EXECUTE;
        r1 = DOSTRUE;
        break;
    case ACTION_COPY_DIR_FH:
    case ACTION_PARENT_FH:
        o = ((struct mfile *)dp->dp_Arg1)->o;
        if (dp->dp_Type == ACTION_PARENT_FH)
            parentOf(&o);
        if (!(r1 = newLock(&o, SHARED_LOCK)))
            r2 = ERROR_NO_FREE_STORE;
        break;
    case ACTION_INFO:
    case ACTION_DISK_INFO:
        info((struct InfoData *)BADDR(dp->dp_Type == ACTION_INFO ? dp->dp_Arg2 : dp->dp_Arg1));
        r1 = DOSTRUE;
        break;
    case ACTION_IS_FILESYSTEM:
    case ACTION_FLUSH:
        r1 = DOSTRUE;
        break;
    case ACTION_CURRENT_VOLUME:
        r1 = 0;
        break;
    case ACTION_DIE:
        r1 = DOSTRUE;
        ReplyPkt(dp, r1, 0);
        return 1;
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE:
    case ACTION_WRITE:
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT:
    case ACTION_CREATE_DIR:
    case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT:
    case ACTION_SET_DATE:
    case ACTION_SET_FILE_SIZE:
    case ACTION_RENAME_DISK:
    case ACTION_FORMAT:
    case ACTION_MAKE_LINK:
    case ACTION_SET_OWNER:
        r1 = dp->dp_Type == ACTION_WRITE ? -1 : DOSFALSE;
        r2 = ERROR_DISK_WRITE_PROTECTED;
        break;
    default:
        r2 = ERROR_ACTION_NOT_KNOWN;
    }
    ReplyPkt(dp, r1, r2);
    return 0;
}

void mh_main(void)
{
    struct Process *me;
    struct Message *msg;
    struct DosPacket *dp;
    struct ExecBase **abs = (struct ExecBase **)4;
    int dying = 0;

    __asm__("" : "+r"(abs));         /* AbsExecBase, without GCC's warning about address 4 */
    SysBase = *abs;
    me = (struct Process *)FindTask(NULL);
    WaitPort(&me->pr_MsgPort);
    msg = GetMsg(&me->pr_MsgPort);
    dp = (struct DosPacket *)msg->mn_Node.ln_Name;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    if (DOSBase)
        port = CreateMsgPort();
    if (!DOSBase || !port) {
        /* Without dos.library 37 there is no ReplyPkt: answer by hand. */
        struct MsgPort *reply = dp->dp_Port;
        dp->dp_Res1 = DOSFALSE;
        dp->dp_Res2 = ERROR_NO_FREE_STORE;
        dp->dp_Port = &me->pr_MsgPort;
        PutMsg(reply, dp->dp_Link);
        if (DOSBase)
            CloseLibrary((struct Library *)DOSBase);
        return;
    }
    devNode = (struct DeviceNode *)BADDR(dp->dp_Arg3);
    cstr(devNode->dn_Name, devName, sizeof devName);
    devNode->dn_Task = port;
    ReplyPkt(dp, DOSTRUE, 0);

    for (;;) {
        WaitPort(port);
        while ((msg = GetMsg(port))) {
            dp = (struct DosPacket *)msg->mn_Node.ln_Name;
            if (packet(dp))
                dying = 1;
        }
        if (dying && !locks && !files)
            break;
    }
    Forbid();
    devNode->dn_Task = NULL;
    while ((msg = GetMsg(port))) {
        dp = (struct DosPacket *)msg->mn_Node.ln_Name;
        ReplyPkt(dp, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
    }
    Permit();
    serviceClose();
    if (io)
        DeleteIORequest((struct IORequest *)io);
    if (ioPort)
        DeleteMsgPort(ioPort);
    DeleteMsgPort(port);
    CloseLibrary((struct Library *)DOSBase);
}
