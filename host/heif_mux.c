/*
 * HEIF's container around HEVC (heif_mux.h), from ISO/IEC 23008-12 (HEIF),
 * ISO/IEC 14496-12 (the ISO base media file format's boxes) and 14496-15
 * (hvcC). The layout is the plainest a reader takes:
 *
 *   ftyp  heic, compatible mif1 heic
 *   meta  hdlr pict, pitm 1, iloc, iinf (item 1 hvc1, item 2 hvc1 for alpha),
 *         iref auxl 2 -> 1 (alpha), iprp (ipco: hvcC, ispe, colr nclx, pixi,
 *         clap when the coded size is bigger, auxC for alpha; ipma)
 *   mdat  each item's slices, a four-byte length before each NAL unit
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "heif_mux.h"

#include <stdlib.h>
#include <string.h>

/* ---- HEVC ------------------------------------------------------------------ */

#define NAL_VPS 32
#define NAL_SPS 33
#define NAL_PPS 34

struct nal {
    const uint8_t *p;
    size_t n;
};

#define MAX_NALS 64

struct stream {
    struct nal vps, sps, pps;
    struct nal vcl[MAX_NALS];
    int nvcl;
};

/* Splits Annex B into NAL units: parameter sets kept by kind, slices (types
 * 0 to 31) in order; SEI, delimiters and the rest left out. */
static int split(const uint8_t *d, size_t n, struct stream *s)
{
    size_t i = 0, start = 0;
    int have = 0;

    memset(s, 0, sizeof *s);
    for (;;) {
        /* the next start code at or after i, or the end */
        size_t next = n, sc = 0;
        for (size_t k = i; k + 2 < n; k++)
            if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) {
                next = k;
                sc = 3;
                break;
            }
        if (have) {
            size_t end = next;
            while (end > start && d[end - 1] == 0)          /* trailing zeros (and a four-byte code's first) */
                end--;
            if (end >= start + 2) {
                struct nal u = { d + start, end - start };
                int type = (d[start] >> 1) & 0x3f;
                if (type == NAL_VPS && !s->vps.p)
                    s->vps = u;
                else if (type == NAL_SPS && !s->sps.p)
                    s->sps = u;
                else if (type == NAL_PPS && !s->pps.p)
                    s->pps = u;
                else if (type < 32) {
                    if (s->nvcl == MAX_NALS)
                        return -1;
                    s->vcl[s->nvcl++] = u;
                }
            }
        }
        if (next == n)
            break;
        start = next + sc;
        i = start;
        have = 1;
    }
    return s->vps.p && s->sps.p && s->pps.p && s->nvcl ? 0 : -1;
}

struct bits {
    uint8_t *d;              /* the RBSP: emulation prevention bytes taken out */
    size_t n, pos;           /* pos in bits */
    int bad;
};

static uint32_t u(struct bits *b, int k)
{
    uint32_t v = 0;
    while (k--) {
        if (b->pos >= b->n * 8) {
            b->bad = 1;
            return 0;
        }
        v = v << 1 | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static uint32_t ue(struct bits *b)
{
    int zeros = 0;
    while (!b->bad && u(b, 1) == 0)
        if (++zeros > 31) {
            b->bad = 1;
            return 0;
        }
    return zeros ? ((1u << zeros) - 1) + u(b, zeros) : 0;
}

int hevc_parse_sps(const uint8_t *nal, size_t n, struct hevc_sps_info *s)
{
    struct bits b = { 0 };
    uint32_t maxsub, i, prof[8], lev[8];
    int rc = -1;

    if (n < 2 + 13 || ((nal[0] >> 1) & 0x3f) != NAL_SPS || !(b.d = malloc(n)))
        return -1;
    for (size_t k = 2, z = 0; k < n; k++) {             /* 00 00 03 -> 00 00 */
        if (z >= 2 && nal[k] == 3) {
            z = 0;
            continue;
        }
        b.d[b.n++] = nal[k];
        z = nal[k] ? 0 : z + 1;
    }
    memset(s, 0, sizeof *s);
    u(&b, 4);                                            /* sps_video_parameter_set_id */
    maxsub = u(&b, 3);
    s->sub_layers = maxsub + 1;
    s->temporal_id_nested = u(&b, 1);
    for (i = 0; i < 12; i++)                             /* general profile, tier and level: 96 bits */
        s->ptl[i] = (uint8_t)u(&b, 8);
    for (i = 0; i < maxsub; i++) {
        prof[i] = u(&b, 1);
        lev[i] = u(&b, 1);
    }
    if (maxsub)
        for (i = maxsub; i < 8; i++)
            u(&b, 2);
    for (i = 0; i < maxsub; i++) {
        if (prof[i])
            b.pos += 88;
        if (lev[i])
            b.pos += 8;
    }
    ue(&b);                                              /* sps_seq_parameter_set_id */
    s->chroma_format = ue(&b);
    if (s->chroma_format == 3)
        u(&b, 1);
    s->width = ue(&b);
    s->height = ue(&b);
    if (u(&b, 1))
        for (i = 0; i < 4; i++)
            ue(&b);
    s->bit_depth_luma = ue(&b) + 8;
    s->bit_depth_chroma = ue(&b) + 8;
    if (!b.bad && s->chroma_format <= 3 && s->width && s->height)
        rc = 0;
    free(b.d);
    return rc;
}

/* ---- boxes ------------------------------------------------------------------- */

struct out {
    uint8_t *d;
    size_t n, cap;
    int bad;
};

static void put(struct out *o, const void *p, size_t n)
{
    if (o->bad)
        return;
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        uint8_t *more;
        while (cap < o->n + n)
            cap *= 2;
        if (!(more = realloc(o->d, cap))) {
            o->bad = 1;
            return;
        }
        o->d = more;
        o->cap = cap;
    }
    memcpy(o->d + o->n, p, n);
    o->n += n;
}

static void p8(struct out *o, uint32_t v) { uint8_t b = (uint8_t)v; put(o, &b, 1); }
static void p16(struct out *o, uint32_t v) { uint8_t b[2] = { v >> 8, v }; put(o, b, 2); }
static void p32(struct out *o, uint32_t v) { uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; put(o, b, 4); }
static void pstr(struct out *o, const char *s) { put(o, s, strlen(s)); }

static void set32(struct out *o, size_t at, uint32_t v)
{
    if (!o->bad) {
        o->d[at] = v >> 24; o->d[at + 1] = v >> 16; o->d[at + 2] = v >> 8; o->d[at + 3] = v;
    }
}

/* A box: its size is filled in by end(). */
static size_t box(struct out *o, const char *type)
{
    size_t at = o->n;
    p32(o, 0);
    pstr(o, type);
    return at;
}

static size_t fullbox(struct out *o, const char *type, uint32_t version, uint32_t flags)
{
    size_t at = box(o, type);
    p32(o, version << 24 | (flags & 0xffffff));
    return at;
}

static void end(struct out *o, size_t at)
{
    set32(o, at, (uint32_t)(o->n - at));
}

static void hvcc(struct out *o, const struct stream *s, const struct hevc_sps_info *i)
{
    const struct nal *sets[3] = { &s->vps, &s->sps, &s->pps };
    size_t at = box(o, "hvcC");
    int k;

    p8(o, 1);                                            /* configurationVersion */
    put(o, i->ptl, 12);                                  /* profile space .. general_level_idc */
    p16(o, 0xf000);                                      /* min_spatial_segmentation_idc 0 */
    p8(o, 0xfc);                                         /* parallelismType 0 */
    p8(o, 0xfc | (i->chroma_format & 3));
    p8(o, 0xf8 | ((i->bit_depth_luma - 8) & 7));
    p8(o, 0xf8 | ((i->bit_depth_chroma - 8) & 7));
    p16(o, 0);                                           /* avgFrameRate */
    /* constantFrameRate 0, numTemporalLayers, temporalIdNested, lengthSizeMinusOne 3 */
    p8(o, (i->sub_layers & 7) << 3 | (i->temporal_id_nested & 1) << 2 | 3);
    p8(o, 3);                                            /* numOfArrays */
    for (k = 0; k < 3; k++) {
        p8(o, 0x80 | (NAL_VPS + k));                     /* array_completeness 1 */
        p16(o, 1);
        p16(o, (uint32_t)sets[k]->n);
        put(o, sets[k]->p, sets[k]->n);
    }
    end(o, at);
}

static void ispe(struct out *o, uint32_t w, uint32_t h)
{
    size_t at = fullbox(o, "ispe", 0, 0);
    p32(o, w);
    p32(o, h);
    end(o, at);
}

static void pixi(struct out *o, int channels, uint32_t depth)
{
    size_t at = fullbox(o, "pixi", 0, 0);
    p8(o, channels);
    for (int c = 0; c < channels; c++)
        p8(o, depth);
    end(o, at);
}

/* The picture's own size, centred crop from the coded one. */
static void clap(struct out *o, const struct heif_picture *p)
{
    size_t at = box(o, "clap");
    p32(o, p->width);  p32(o, 1);                        /* cleanApertureWidth N/D */
    p32(o, p->height); p32(o, 1);
    /* the offsets of its centre from the coded picture's centre: (w - cw) / 2 */
    p32(o, (uint32_t)((int32_t)p->width - (int32_t)p->coded_width));  p32(o, 2);
    p32(o, (uint32_t)((int32_t)p->height - (int32_t)p->coded_height)); p32(o, 2);
    end(o, at);
}

static size_t slices(const struct stream *s)
{
    size_t n = 0;
    for (int k = 0; k < s->nvcl; k++)
        n += 4 + s->vcl[k].n;
    return n;
}

int heif_write(const struct heif_picture *p, uint8_t **out, size_t *len)
{
    struct stream pic, alpha;
    struct hevc_sps_info pi, alpha_info;
    struct out o = { 0 };
    int has_alpha = p->alpha != NULL, crop, k;
    size_t at, meta, ipco, ipma, iloc_offsets[2] = { 0, 0 }, mdat;
    /* ipco's properties, 1-based, in the order they are written */
    enum { P_HVCC = 1, P_ISPE, P_COLR, P_PIXI, P_CLAP_OR_AHVCC };
    int ahvcc, apixi, auxc, pclap = 0;

    if (split(p->hevc, p->hevc_len, &pic) || hevc_parse_sps(pic.sps.p, pic.sps.n, &pi))
        return -1;
    if (has_alpha && (split(p->alpha, p->alpha_len, &alpha) || hevc_parse_sps(alpha.sps.p, alpha.sps.n, &alpha_info)))
        return -1;
    if (!p->width || !p->height || p->width > p->coded_width || p->height > p->coded_height)
        return -1;
    crop = p->width != p->coded_width || p->height != p->coded_height;

    at = box(&o, "ftyp");
    pstr(&o, "heic");
    p32(&o, 0);
    pstr(&o, "mif1");
    pstr(&o, "heic");
    end(&o, at);

    meta = fullbox(&o, "meta", 0, 0);
    at = fullbox(&o, "hdlr", 0, 0);
    p32(&o, 0);
    pstr(&o, "pict");
    p32(&o, 0); p32(&o, 0); p32(&o, 0);
    p8(&o, 0);
    end(&o, at);

    at = fullbox(&o, "pitm", 0, 0);
    p16(&o, 1);
    end(&o, at);

    /* iloc v0: offset and length 4 bytes, no base offset; offsets filled in below */
    at = fullbox(&o, "iloc", 0, 0);
    p8(&o, 0x44);
    p8(&o, 0x00);
    p16(&o, has_alpha ? 2 : 1);
    for (k = 0; k < (has_alpha ? 2 : 1); k++) {
        p16(&o, k + 1);
        p16(&o, 0);                                      /* data_reference_index: this file */
        p16(&o, 1);                                      /* one extent */
        iloc_offsets[k] = o.n;
        p32(&o, 0);
        p32(&o, (uint32_t)slices(k ? &alpha : &pic));
    }
    end(&o, at);

    at = fullbox(&o, "iinf", 0, 0);
    p16(&o, has_alpha ? 2 : 1);
    for (k = 0; k < (has_alpha ? 2 : 1); k++) {
        size_t e = fullbox(&o, "infe", 2, 0);
        p16(&o, k + 1);
        p16(&o, 0);
        pstr(&o, "hvc1");
        p8(&o, 0);                                       /* item_name "" */
        end(&o, e);
    }
    end(&o, at);

    if (has_alpha) {
        at = fullbox(&o, "iref", 0, 0);
        size_t r = box(&o, "auxl");
        p16(&o, 2);
        p16(&o, 1);
        p16(&o, 1);
        end(&o, r);
        end(&o, at);
    }

    at = box(&o, "iprp");
    ipco = box(&o, "ipco");
    hvcc(&o, &pic, &pi);                                 /* 1 */
    ispe(&o, p->coded_width, p->coded_height);           /* 2 */
    {                                                    /* 3: sRGB primaries and transfer, BT.601 matrix */
        size_t c = box(&o, "colr");
        pstr(&o, "nclx");
        p16(&o, 1);                                      /* BT.709 primaries: sRGB's */
        p16(&o, 13);                                     /* sRGB transfer */
        p16(&o, 6);                                      /* BT.601 matrix (FFmpeg's RGB to YCbCr) */
        p8(&o, p->full_range ? 0x80 : 0);
        end(&o, c);
    }
    pixi(&o, 3, pi.bit_depth_luma);                      /* 4 */
    k = P_CLAP_OR_AHVCC;
    if (crop) {
        clap(&o, p);
        pclap = k++;
    }
    ahvcc = apixi = auxc = 0;
    if (has_alpha) {
        hvcc(&o, &alpha, &alpha_info);
        ahvcc = k++;
        pixi(&o, 1, alpha_info.bit_depth_luma);
        apixi = k++;
        size_t c = fullbox(&o, "auxC", 0, 0);
        pstr(&o, "urn:mpeg:hevc:2015:auxid:1");
        p8(&o, 0);
        end(&o, c);
        auxc = k++;
    }
    end(&o, ipco);

    ipma = fullbox(&o, "ipma", 0, 0);
    p32(&o, has_alpha ? 2 : 1);
    p16(&o, 1);
    p8(&o, 4 + (crop ? 1 : 0));
    p8(&o, 0x80 | P_HVCC);                               /* essential */
    p8(&o, P_ISPE);
    p8(&o, P_COLR);
    p8(&o, P_PIXI);
    if (crop)
        p8(&o, 0x80 | pclap);
    if (has_alpha) {
        p16(&o, 2);
        p8(&o, 4 + (crop ? 1 : 0));
        p8(&o, 0x80 | ahvcc);
        p8(&o, P_ISPE);
        p8(&o, apixi);
        p8(&o, 0x80 | auxc);
        if (crop)
            p8(&o, 0x80 | pclap);
    }
    end(&o, ipma);
    end(&o, at);
    end(&o, meta);

    mdat = box(&o, "mdat");
    for (k = 0; k < (has_alpha ? 2 : 1); k++) {
        const struct stream *s = k ? &alpha : &pic;
        set32(&o, iloc_offsets[k], (uint32_t)o.n);
        for (int j = 0; j < s->nvcl; j++) {
            p32(&o, (uint32_t)s->vcl[j].n);
            put(&o, s->vcl[j].p, s->vcl[j].n);
        }
    }
    end(&o, mdat);
    if (o.bad || o.n > 0xffffffffu) {
        free(o.d);
        return -1;
    }
    *out = o.d;
    *len = o.n;
    return 0;
}
