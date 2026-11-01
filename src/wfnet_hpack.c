/*
 * wfnet: HPACK (RFC 7541) for its HTTP/2: the static table, a dynamic
 * table each way, integers and strings, and the Huffman code of Appendix B.
 *
 * The Huffman code is canonical (its codes are given by their lengths, in
 * order of length and then symbol), so only the lengths are kept, with the
 * symbols in code order for decoding: about 0.5 KB in place of a table of
 * codes. Decoding goes a bit at a time; header blocks are small.
 *
 * A dynamic table is one buffer, oldest entry first, each entry its name,
 * its value and then their two lengths, so the newest is found from the
 * end. Evicting moves what is left down (at most the table's size).
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "openweb/wfnet.h"
#include "wfnet_int.h"

/* RFC 7541 Appendix B: each symbol's code length (256 is EOS). */
static const unsigned char huff_len[257] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28, 28, 28, 28, 28,
    28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28, 6, 10, 10, 12, 13, 6, 8, 11,
    10, 10, 8, 11, 8, 6, 6, 6, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8,
    15, 6, 12, 10, 13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6, 15, 5, 6, 5,
    6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5, 6, 7, 6, 5, 5, 6, 7, 7,
    7, 7, 7, 15, 11, 14, 13, 28, 20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23,
    23, 23, 24, 23, 24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23, 21, 21, 22, 21,
    23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23, 26, 26, 20, 19, 22, 23, 22, 25,
    26, 26, 26, 27, 27, 26, 24, 25, 19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26,
    28, 27, 27, 27, 20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26, 30
};
/* The symbols in code order (by length, then value): the code is canonical. */
static const unsigned char huff_sym[256] = {
    48, 49, 50, 97, 99, 101, 105, 111, 115, 116, 32, 37, 45, 46, 47, 51, 52, 53, 54, 55,
    56, 57, 61, 65, 95, 98, 100, 102, 103, 104, 108, 109, 110, 112, 114, 117, 58, 66, 67, 68,
    69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 89,
    106, 107, 113, 118, 119, 120, 121, 122, 38, 42, 44, 59, 88, 90, 33, 34, 40, 41, 63, 39,
    43, 124, 35, 62, 0, 36, 64, 91, 93, 126, 94, 125, 60, 96, 123, 92, 195, 208, 128, 130,
    131, 162, 184, 194, 224, 226, 153, 161, 167, 172, 176, 177, 179, 209, 216, 217, 227, 229, 230, 129,
    132, 133, 134, 136, 146, 154, 156, 160, 163, 164, 169, 170, 173, 178, 181, 185, 186, 187, 189, 190,
    196, 198, 228, 232, 233, 1, 135, 137, 138, 139, 140, 141, 143, 147, 149, 150, 151, 152, 155, 157,
    158, 165, 166, 168, 174, 175, 180, 182, 183, 188, 191, 197, 231, 239, 9, 142, 144, 145, 148, 159,
    171, 206, 215, 225, 236, 237, 199, 207, 234, 235, 192, 193, 200, 201, 202, 205, 210, 213, 218, 219,
    238, 240, 242, 243, 255, 203, 204, 211, 212, 214, 221, 222, 223, 241, 244, 245, 246, 247, 248, 250,
    251, 252, 253, 254, 2, 3, 4, 5, 6, 7, 8, 11, 12, 14, 15, 16, 17, 18, 19, 20,
    21, 23, 24, 25, 26, 27, 28, 29, 30, 31, 127, 220, 249, 10, 13, 22
};

/* Appendix A: the static table, each entry its name and value, NUL ended. */
static const char static_table[] =
    ":authority\0\0:method\0GET\0:method\0POST\0:path\0/\0:path\0/index.html\0:scheme\0http\0:scheme\0https\0"
    ":status\0" "200\0:status\0" "204\0:status\0" "206\0:status\0" "304\0:status\0" "400\0:status\0" "404\0"
    ":status\0" "500\0accept-charset\0\0accept-encoding\0gzip, deflate\0accept-language\0\0accept-ranges\0\0"
    "accept\0\0access-control-allow-origin\0\0age\0\0allow\0\0authorization\0\0cache-control\0\0"
    "content-disposition\0\0content-encoding\0\0content-language\0\0content-length\0\0content-location\0\0"
    "content-range\0\0content-type\0\0cookie\0\0date\0\0etag\0\0expect\0\0expires\0\0from\0\0host\0\0if-match\0\0"
    "if-modified-since\0\0if-none-match\0\0if-range\0\0if-unmodified-since\0\0last-modified\0\0link\0\0"
    "location\0\0max-forwards\0\0proxy-authenticate\0\0proxy-authorization\0\0range\0\0referer\0\0refresh\0\0"
    "retry-after\0\0server\0\0set-cookie\0\0strict-transport-security\0\0transfer-encoding\0\0user-agent\0\0"
    "vary\0\0via\0\0www-authenticate\0";
#define STATIC_COUNT 61

struct wfn_hpack {
    unsigned char *buf;         /* the entries, oldest first */
    unsigned long used;         /* bytes of buf in use */
    unsigned long size;         /* the table's size as RFC 7541 counts it (32 more per entry) */
    unsigned long max;          /* its limit now */
    unsigned long cap;          /* the limit it was made with: buf's room */
    unsigned long count;
};

struct entry {
    const char *name, *value;
    unsigned long nl, vl;
};

static void static_entry(unsigned long i, struct entry *e)
{
    const char *p = static_table;
    while (--i) {
        p += strlen(p) + 1;
        p += strlen(p) + 1;
    }
    e->name = p;
    e->nl = strlen(p);
    e->value = p + e->nl + 1;
    e->vl = strlen(e->value);
}

/* Index 1 the static table's first; STATIC_COUNT + 1 the newest dynamic. 0 when there is none. */
static int lookup(const struct wfn_hpack *h, unsigned long i, struct entry *e)
{
    unsigned long at;
    if (!i)
        return 0;
    if (i <= STATIC_COUNT) {
        static_entry(i, e);
        return 1;
    }
    i -= STATIC_COUNT;
    if (!h || i > h->count)
        return 0;
    at = h->used;
    for (;;) {
        e->nl = (unsigned long)h->buf[at - 4] << 8 | h->buf[at - 3];
        e->vl = (unsigned long)h->buf[at - 2] << 8 | h->buf[at - 1];
        at -= 4 + e->nl + e->vl;
        if (!--i)
            break;
    }
    e->name = (const char *)h->buf + at;
    e->value = e->name + e->nl;
    return 1;
}

/* Drops the oldest entries until the size is at most max. */
static void evict(struct wfn_hpack *h, unsigned long max)
{
    while (h->size > max && h->count) {
        /* The oldest entry starts at 0; its lengths follow it, so find its end
         * by walking from the newest, whose end is known. */
        unsigned long at = h->used, prev = h->used, n;
        while (at) {
            unsigned long l = ((unsigned long)h->buf[at - 4] << 8 | h->buf[at - 3]) +
                              ((unsigned long)h->buf[at - 2] << 8 | h->buf[at - 1]);
            prev = at;
            at -= 4 + l;
        }
        n = prev;                                   /* the oldest's bytes: 0 to prev */
        h->size -= n - 4 + 32;
        memmove(h->buf, h->buf + n, h->used - n);
        h->used -= n;
        h->count--;
    }
}

static void add(struct wfn_hpack *h, const char *name, unsigned long nl, const char *value, unsigned long vl)
{
    unsigned long need = nl + vl + 32;
    if (need > h->max) {                            /* larger than the table: it empties it */
        evict(h, 0);
        return;
    }
    evict(h, h->max - need);
    memmove(h->buf + h->used, name, nl);
    memmove(h->buf + h->used + nl, value, vl);
    h->used += nl + vl;
    h->buf[h->used++] = (unsigned char)(nl >> 8);
    h->buf[h->used++] = (unsigned char)nl;
    h->buf[h->used++] = (unsigned char)(vl >> 8);
    h->buf[h->used++] = (unsigned char)vl;
    h->size += need;
    h->count++;
}

struct wfn_hpack *wfn_hpack_new(const struct wfn_io *io, unsigned long table)
{
    struct wfn_hpack *h = io->alloc(io->env, sizeof *h + table);
    if (!h)
        return NULL;
    h->buf = (unsigned char *)(h + 1);
    h->max = h->cap = table;
    return h;
}

void wfn_hpack_free(const struct wfn_io *io, struct wfn_hpack *h)
{
    if (h)
        io->release(io->env, h);
}

unsigned long wfn_hpack_table_size(const struct wfn_hpack *h)
{
    return h->size;
}

/* ---- Huffman ------------------------------------------------------------ */

/* Each length's first code, its count, and where its symbols start in huff_sym. */
struct canon {
    wu32 first[31];
    unsigned short count[31], offset[31];
};

static void canon(struct canon *c)
{
    wu32 code = 0;
    unsigned i, at = 0;
    memset(c, 0, sizeof *c);
    for (i = 0; i < 257; i++)
        c->count[huff_len[i]]++;
    for (i = 1; i < 31; i++) {
        code = (code + (i > 1 ? c->count[i - 1] : 0)) << 1;
        c->first[i] = code;
        c->offset[i] = (unsigned short)at;
        at += c->count[i];
    }
}

unsigned long wfn_huff_encode(const unsigned char *in, unsigned long n, unsigned char *out, unsigned long room)
{
    struct canon c;
    unsigned char rank[257];
    wu32 acc = 0;
    unsigned long i, o = 0;
    unsigned bits = 0, k;
    canon(&c);
    /* Each symbol's place among those of its length. */
    for (k = 0; k < 256; k++)
        rank[huff_sym[k]] = (unsigned char)(k - c.offset[huff_len[huff_sym[k]]]);
    for (i = 0; i < n; i++) {
        unsigned len = huff_len[in[i]];
        wu32 code = c.first[len] + rank[in[i]];
        /* At most 30 bits at a time: put out whole bytes as they fill. */
        while (len) {
            unsigned take = len > 8 ? 8 : len;
            acc = acc << take | ((code >> (len - take)) & ((1U << take) - 1));
            bits += take;
            len -= take;
            if (bits >= 8) {
                if (o >= room)
                    return 0;
                out[o++] = (unsigned char)(acc >> (bits - 8));
                bits -= 8;
            }
        }
    }
    if (bits) {                                     /* padded with the EOS code's leading ones */
        if (o >= room)
            return 0;
        out[o++] = (unsigned char)(acc << (8 - bits) | (0xffU >> bits));
    }
    return o;
}

/* The length of in's Huffman code, in bytes. */
static unsigned long huff_length(const unsigned char *in, unsigned long n)
{
    unsigned long bits = 0, i;
    for (i = 0; i < n; i++)
        bits += huff_len[in[i]];
    return (bits + 7) >> 3;
}

long wfn_huff_decode(const unsigned char *in, unsigned long n, unsigned char *out, unsigned long room)
{
    struct canon c;
    wu32 code = 0;
    unsigned len = 0, ones = 1;
    unsigned long i, o = 0;
    canon(&c);
    for (i = 0; i < n * 8; i++) {
        unsigned bit = (in[i >> 3] >> (7 - (i & 7))) & 1;
        code = code << 1 | bit;
        ones &= bit;
        len++;
        if (len >= 5 && code - c.first[len] < c.count[len]) {
            unsigned sym = c.offset[len] + (unsigned)(code - c.first[len]);
            if (sym >= 256)                         /* EOS in a string is an error */
                return -1;
            if (o >= room)
                return -1;
            out[o++] = huff_sym[sym];
            code = 0, len = 0, ones = 1;
        } else if (len >= 30)
            return -1;
    }
    /* What is left must be fewer than 8 bits, all ones (the EOS code's start). */
    if (len > 7 || !ones)
        return -1;
    return (long)o;
}

/* ---- integers and strings ----------------------------------------------- */

/* An integer with an n-bit prefix (5.1): its bytes, or 0 when out of room. */
static unsigned long put_int(unsigned char *out, unsigned long room, unsigned first, unsigned n, unsigned long v)
{
    unsigned long o = 0, limit = (1UL << n) - 1;
    if (!room)
        return 0;
    if (v < limit) {
        out[0] = (unsigned char)(first | v);
        return 1;
    }
    out[o++] = (unsigned char)(first | limit);
    v -= limit;
    while (v >= 128) {
        if (o >= room)
            return 0;
        out[o++] = (unsigned char)(v | 128);
        v >>= 7;
    }
    if (o >= room)
        return 0;
    out[o++] = (unsigned char)v;
    return o;
}

/* Reads an integer with an n-bit prefix at *at: 0, or -1 when cut off or too big. */
static int get_int(const unsigned char *in, unsigned long len, unsigned long *at, unsigned n, unsigned long *v)
{
    unsigned long limit = (1UL << n) - 1, m = 0;
    unsigned shift = 0;
    if (*at >= len)
        return -1;
    *v = in[(*at)++] & limit;
    if (*v < limit)
        return 0;
    do {
        if (*at >= len || shift > 21)
            return -1;
        m = in[*at];
        *v += (m & 127) << shift;
        shift += 7;
    } while (in[(*at)++] & 128);
    return 0;
}

static unsigned long put_str(unsigned char *out, unsigned long room, const char *s, unsigned long n, int huffman)
{
    unsigned long hl = huffman ? huff_length((const unsigned char *)s, n) : n, o;
    if (huffman && hl > n)
        huffman = 0, hl = n;
    if (!(o = put_int(out, room, huffman ? 0x80 : 0, 7, hl)) || o + hl > room)
        return 0;
    if (huffman)
        wfn_huff_encode((const unsigned char *)s, n, out + o, hl);
    else
        memcpy(out + o, s, n);
    return o + hl;
}

/* A string at *at into out (at most room bytes): its length, or -1. */
static long get_str(const unsigned char *in, unsigned long len, unsigned long *at, char *out, unsigned long room)
{
    unsigned long n;
    int huffman;
    long got;
    if (*at >= len)
        return -1;
    huffman = in[*at] & 0x80;
    if (get_int(in, len, at, 7, &n) || n > len - *at)
        return -1;
    if (huffman)
        got = wfn_huff_decode(in + *at, n, (unsigned char *)out, room);
    else if (n > room)
        got = -1;
    else
        memcpy(out, in + *at, n), got = (long)n;
    *at += n;
    return got;
}

/* ---- header blocks ------------------------------------------------------- */

long wfn_hpack_decode(struct wfn_hpack *h, const unsigned char *in, unsigned long n, char *out, unsigned long room)
{
    unsigned long at = 0, o = 0;
    int fields = 0;
    while (at < n) {
        unsigned b = in[at];
        struct entry e;
        unsigned long idx;
        long nl, vl;
        char *name, *value;
        if ((b & 0xe0) == 0x20) {                   /* 6.3: dynamic table size update */
            if (get_int(in, n, &at, 5, &idx) || idx > h->cap || fields)
                return -1;
            h->max = idx;
            evict(h, idx);
            continue;
        }
        fields++;
        if (b & 0x80) {                             /* 6.1: indexed */
            if (get_int(in, n, &at, 7, &idx) || !lookup(h, idx, &e))
                return -1;
            if (o + e.nl + e.vl + 3 > room)
                return -1;
            memcpy(out + o, e.name, e.nl);
            o += e.nl;
            out[o++] = ':', out[o++] = ' ';
            memcpy(out + o, e.value, e.vl);
            o += e.vl;
            out[o++] = '\n';
            continue;
        }
        /* 6.2: a literal; with incremental indexing (01), without (0000) or never (0001). */
        if (get_int(in, n, &at, (b & 0x40) ? 6 : 4, &idx))
            return -1;
        name = out + o;
        if (idx) {
            if (!lookup(h, idx, &e) || o + e.nl + 2 > room)
                return -1;
            memcpy(name, e.name, e.nl);
            nl = (long)e.nl;
        } else if ((nl = get_str(in, n, &at, name, room - o)) < 0)
            return -1;
        if (o + (unsigned long)nl + 3 > room)
            return -1;
        name[nl] = ':', name[nl + 1] = ' ';
        value = name + nl + 2;
        if ((vl = get_str(in, n, &at, value, room - o - (unsigned long)nl - 3)) < 0)
            return -1;
        if (b & 0x40)
            add(h, name, (unsigned long)nl, value, (unsigned long)vl);
        o += (unsigned long)nl + 2 + (unsigned long)vl;
        out[o++] = '\n';
    }
    return (long)o;
}

static int same(const char *a, const char *b, unsigned long n)
{
    return !memcmp(a, b, n);
}

unsigned long wfn_hpack_encode(struct wfn_hpack *h, const char *name, unsigned long nl, const char *value,
                               unsigned long vl, int index, int huffman, unsigned char *out, unsigned long room)
{
    unsigned long i, name_at = 0, o, s;
    const char *p = static_table;
    struct entry e;
    for (i = 1; i <= STATIC_COUNT + (h ? h->count : 0); i++) {
        if (i <= STATIC_COUNT) {                    /* the static table, walked in order */
            e.name = p, e.nl = strlen(p), e.value = p + e.nl + 1, e.vl = strlen(e.value);
            p = e.value + e.vl + 1;
        } else
            lookup(h, i, &e);
        if (e.nl != nl || !same(e.name, name, nl))
            continue;
        if (e.vl == vl && same(e.value, value, vl))
            return put_int(out, room, 0x80, 7, i);
        if (!name_at)
            name_at = i;
    }
    index = index && h;
    if (!(o = put_int(out, room, index ? 0x40 : 0, index ? 6 : 4, name_at)))
        return 0;
    if (!name_at) {
        if (!(s = put_str(out + o, room - o, name, nl, huffman)))
            return 0;
        o += s;
    }
    if (!(s = put_str(out + o, room - o, value, vl, huffman)))
        return 0;
    if (index)
        add(h, name, nl, value, vl);
    return o + s;
}
