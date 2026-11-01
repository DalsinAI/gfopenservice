/*
 * wfnet: the hashes and encodings its protocols need (MD5 from RFC 1321,
 * SHA-1 and SHA-256 from FIPS 180-4, base64 from RFC 4648), the
 * WebSocket accept key (RFC 6455) and the answers to HTTP Basic (RFC 7617)
 * and Digest (RFC 7616, MD5 and SHA-256, with or without -sess)
 * challenges. Small and plain: a few hashes per request, never bulk data.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "openweb/wfnet.h"
#include "wfnet_int.h"

/* ---- MD5, SHA-1, SHA-256 behind one streaming interface --------------- */

enum { H_MD5, H_SHA1, H_SHA256 };

struct hctx {
    wu32 h[8];
    unsigned char buf[64];
    wu32 n, nhi;               /* bytes so far */
    int kind;
};

static const wu32 md5_t[64] = {
    0xd76aa478UL, 0xe8c7b756UL, 0x242070dbUL, 0xc1bdceeeUL, 0xf57c0fafUL, 0x4787c62aUL, 0xa8304613UL, 0xfd469501UL,
    0x698098d8UL, 0x8b44f7afUL, 0xffff5bb1UL, 0x895cd7beUL, 0x6b901122UL, 0xfd987193UL, 0xa679438eUL, 0x49b40821UL,
    0xf61e2562UL, 0xc040b340UL, 0x265e5a51UL, 0xe9b6c7aaUL, 0xd62f105dUL, 0x02441453UL, 0xd8a1e681UL, 0xe7d3fbc8UL,
    0x21e1cde6UL, 0xc33707d6UL, 0xf4d50d87UL, 0x455a14edUL, 0xa9e3e905UL, 0xfcefa3f8UL, 0x676f02d9UL, 0x8d2a4c8aUL,
    0xfffa3942UL, 0x8771f681UL, 0x6d9d6122UL, 0xfde5380cUL, 0xa4beea44UL, 0x4bdecfa9UL, 0xf6bb4b60UL, 0xbebfbc70UL,
    0x289b7ec6UL, 0xeaa127faUL, 0xd4ef3085UL, 0x04881d05UL, 0xd9d4d039UL, 0xe6db99e5UL, 0x1fa27cf8UL, 0xc4ac5665UL,
    0xf4292244UL, 0x432aff97UL, 0xab9423a7UL, 0xfc93a039UL, 0x655b59c3UL, 0x8f0ccc92UL, 0xffeff47dUL, 0x85845dd1UL,
    0x6fa87e4fUL, 0xfe2ce6e0UL, 0xa3014314UL, 0x4e0811a1UL, 0xf7537e82UL, 0xbd3af235UL, 0x2ad7d2bbUL, 0xeb86d391UL
};
static const unsigned char md5_r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static const wu32 sha256_k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};
static const wu32 sha256_h0[8] = {
    0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL, 0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
};

#define ROL(x, n) ((wu32)(((x) << (n)) | ((x) >> (32 - (n)))))
#define ROR(x, n) ((wu32)(((x) >> (n)) | ((x) << (32 - (n)))))

static wu32 be32(const unsigned char *p)
{
    return (wu32)p[0] << 24 | (wu32)p[1] << 16 | (wu32)p[2] << 8 | p[3];
}

static wu32 le32(const unsigned char *p)
{
    return (wu32)p[3] << 24 | (wu32)p[2] << 16 | (wu32)p[1] << 8 | p[0];
}

static void block(struct hctx *c, const unsigned char *p)
{
    wu32 w[64], a, b, d, e, f, g, t, u;
    int i;
    if (c->kind == H_MD5) {
        for (i = 0; i < 16; i++)
            w[i] = le32(p + 4 * i);
        a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3];      /* A B C D */
        for (i = 0; i < 64; i++) {
            int k;
            if (i < 16)
                f = (b & d) | (~b & e), k = i;
            else if (i < 32)
                f = (e & b) | (~e & d), k = (5 * i + 1) & 15;
            else if (i < 48)
                f = b ^ d ^ e, k = (3 * i + 5) & 15;
            else
                f = d ^ (b | ~e), k = (7 * i) & 15;
            t = a + f + md5_t[i] + w[k];
            a = e, e = d, d = b;
            b = b + ROL(t, md5_r[(i >> 4) * 4 + (i & 3)]);
        }
        c->h[0] += a, c->h[1] += b, c->h[2] += d, c->h[3] += e;
        return;
    }
    for (i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    if (c->kind == H_SHA1) {
        for (i = 16; i < 80 && i < 64; i++)
            w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], g = c->h[4];
        for (i = 0; i < 80; i++) {
            wu32 k, wi;
            if (i < 64)
                wi = w[i];
            else {                                     /* the last 16 in place of w[64..79] */
                wi = ROL(w[(i - 3) & 63] ^ w[(i - 8) & 63] ^ w[(i - 14) & 63] ^ w[(i - 16) & 63], 1);
                w[i & 63] = wi;
            }
            if (i < 20)
                f = (b & d) | (~b & e), k = 0x5a827999UL;
            else if (i < 40)
                f = b ^ d ^ e, k = 0x6ed9eba1UL;
            else if (i < 60)
                f = (b & d) | (b & e) | (d & e), k = 0x8f1bbcdcUL;
            else
                f = b ^ d ^ e, k = 0xca62c1d6UL;
            t = ROL(a, 5) + f + g + k + wi;
            g = e, e = d, d = ROL(b, 30), b = a, a = t;
        }
        c->h[0] += a, c->h[1] += b, c->h[2] += d, c->h[3] += e, c->h[4] += g;
        return;
    }
    for (i = 16; i < 64; i++) {
        t = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + t + w[i - 7] + u;
    }
    {
        wu32 s[8];
        for (i = 0; i < 8; i++)
            s[i] = c->h[i];
        for (i = 0; i < 64; i++) {
            t = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + sha256_k[i] + w[i];
            u = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
            s[7] = s[6], s[6] = s[5], s[5] = s[4], s[4] = s[3] + t;
            s[3] = s[2], s[2] = s[1], s[1] = s[0], s[0] = t + u;
        }
        for (i = 0; i < 8; i++)
            c->h[i] += s[i];
    }
}

static void hinit(struct hctx *c, int kind)
{
    static const wu32 md5_sha1_h0[5] = { 0x67452301UL, 0xefcdab89UL, 0x98badcfeUL, 0x10325476UL, 0xc3d2e1f0UL };
    memset(c, 0, sizeof *c);
    c->kind = kind;
    memcpy(c->h, kind == H_SHA256 ? sha256_h0 : md5_sha1_h0, kind == H_SHA256 ? sizeof sha256_h0 : sizeof md5_sha1_h0);
}

static void hupdate(struct hctx *c, const void *data, unsigned long n)
{
    const unsigned char *p = data;
    while (n) {
        unsigned at = c->n & 63, take = 64 - at < n ? 64 - at : (unsigned)n;
        memcpy(c->buf + at, p, take);
        if ((wu32)(c->n + take) < c->n)
            c->nhi++;
        c->n += take, p += take, n -= take;
        if (!(c->n & 63))
            block(c, c->buf);
    }
}

static void hstr(struct hctx *c, const char *s)
{
    hupdate(c, s, strlen(s));
}

/* The digest's length. */
static int hfinal(struct hctx *c, unsigned char *out)
{
    unsigned char pad[72];
    wu32 lo = c->n << 3, hi = c->nhi << 3 | c->n >> 29;
    unsigned at = c->n & 63, padn = (at < 56 ? 56 : 120) - at, i, words = c->kind == H_SHA256 ? 8 : c->kind == H_SHA1 ? 5 : 4;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 4; i++) {
        if (c->kind == H_MD5)
            pad[padn + i] = (unsigned char)(lo >> 8 * i), pad[padn + 4 + i] = (unsigned char)(hi >> 8 * i);
        else
            pad[padn + i] = (unsigned char)(hi >> (24 - 8 * i)), pad[padn + 4 + i] = (unsigned char)(lo >> (24 - 8 * i));
    }
    hupdate(c, pad, padn + 8);
    for (i = 0; i < words * 4; i++)
        out[i] = c->kind == H_MD5 ? (unsigned char)(c->h[i >> 2] >> 8 * (i & 3))
                                  : (unsigned char)(c->h[i >> 2] >> (24 - 8 * (i & 3)));
    return (int)words * 4;
}

static void hash(int kind, const void *p, unsigned long n, unsigned char *out)
{
    struct hctx c;
    hinit(&c, kind);
    hupdate(&c, p, n);
    hfinal(&c, out);
}

void wfn_md5(const void *p, unsigned long n, unsigned char out[16])
{
    hash(H_MD5, p, n, out);
}

void wfn_sha1(const void *p, unsigned long n, unsigned char out[20])
{
    hash(H_SHA1, p, n, out);
}

void wfn_sha256(const void *p, unsigned long n, unsigned char out[32])
{
    hash(H_SHA256, p, n, out);
}

unsigned long wfn_base64(const unsigned char *in, unsigned long n, char *out)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long i, o = 0;
    for (i = 0; i < n; i += 3) {
        wu32 v = (wu32)in[i] << 16 | (i + 1 < n ? (wu32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = abc[v >> 18];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = i + 1 < n ? abc[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? abc[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

void wfn_ws_accept(const char *key, char out[29])
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    struct hctx c;
    unsigned char d[20];
    hinit(&c, H_SHA1);
    hstr(&c, key);
    hstr(&c, guid);
    hfinal(&c, d);
    wfn_base64(d, 20, out);
}

/* ---- HTTP authentication ----------------------------------------------- */

static void hex(const unsigned char *d, int n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++)
        out[2 * i] = digits[d[i] >> 4], out[2 * i + 1] = digits[d[i] & 15];
    out[2 * n] = 0;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int same(const char *a, unsigned long n, const char *b)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        if (!b[i] || lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
    return !b[n];
}

/* One challenge's parameters, pointing into the header's value. */
struct chal {
    const char *scheme, *realm, *nonce, *opaque, *qop, *alg, *userhash;
    unsigned long sl, rl, nl, ol, ql, al, ul;
};

static const char *skip(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
        p++;
    return p;
}

static int tokchar(int c)
{
    return c > ' ' && c < 127 && c != ',' && c != '=' && c != '"' && c != ';';
}

/* Parses one challenge at *pp: 1 and *c, or 0 at the end. A quoted value
 * keeps its backslashes (none of the values used here have any). */
static int challenge(const char **pp, const char *end, struct chal *c)
{
    const char *p = skip(*pp, end);
    memset(c, 0, sizeof *c);
    if (p >= end)
        return 0;
    c->scheme = p;
    while (p < end && tokchar((unsigned char)*p))
        p++;
    c->sl = (unsigned long)(p - c->scheme);
    for (;;) {
        const char *name, *val, *save = p;
        unsigned long nl, vl;
        p = skip(p, end);
        name = p;
        while (p < end && tokchar((unsigned char)*p))
            p++;
        nl = (unsigned long)(p - name);
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (!nl || p >= end || *p != '=') {           /* the next challenge's scheme, or the end */
            *pp = nl ? save : p;
            return c->sl != 0;
        }
        p++;
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p < end && *p == '"') {
            val = ++p;
            while (p < end && *p != '"')
                p += (*p == '\\' && p + 1 < end) ? 2 : 1;
            vl = (unsigned long)(p - val);
            if (p < end)
                p++;
        } else {
            val = p;
            while (p < end && *p != ',' && *p != ' ' && *p != '\t')
                p++;
            vl = (unsigned long)(p - val);
        }
        if (same(name, nl, "realm"))
            c->realm = val, c->rl = vl;
        else if (same(name, nl, "nonce"))
            c->nonce = val, c->nl = vl;
        else if (same(name, nl, "opaque"))
            c->opaque = val, c->ol = vl;
        else if (same(name, nl, "qop"))
            c->qop = val, c->ql = vl;
        else if (same(name, nl, "algorithm"))
            c->alg = val, c->al = vl;
        else if (same(name, nl, "userhash"))
            c->userhash = val, c->ul = vl;
    }
}

/* Whether the comma list holds item (any case). */
static int has_item(const char *list, unsigned long n, const char *item)
{
    const char *p = list, *end = list + n;
    while (p < end) {
        const char *s;
        while (p < end && (*p == ' ' || *p == ','))
            p++;
        s = p;
        while (p < end && *p != ',' && *p != ' ')
            p++;
        if (p > s && same(s, (unsigned long)(p - s), item))
            return 1;
    }
    return 0;
}

/* Digest's algorithms: 1 MD5, 2 SHA-256, each | 4 for -sess; 0 unknown. */
static int digest_alg(const struct chal *c)
{
    if (!c->alg || same(c->alg, c->al, "MD5"))
        return 1;
    if (same(c->alg, c->al, "MD5-sess"))
        return 5;
    if (same(c->alg, c->al, "SHA-256"))
        return 2;
    if (same(c->alg, c->al, "SHA-256-sess"))
        return 6;
    return 0;
}

struct out {
    char *p;
    unsigned long at, room;
};

static void put(struct out *o, const char *s, unsigned long n)
{
    if (o->at + n < o->room)
        memcpy(o->p + o->at, s, n);
    o->at += n;
}

static void puts_(struct out *o, const char *s)
{
    put(o, s, strlen(s));
}

static void hcolon(struct hctx *c, const char *s, unsigned long n)
{
    hupdate(c, s, n);
    hupdate(c, ":", 1);
}

unsigned long wfn_auth_answer(const char *value, unsigned long vlen, const char *user, const char *password,
                              const char *method, const char *uri, unsigned long nc, const char *cnonce,
                              char *out, unsigned long room)
{
    const char *p = value, *end = value + vlen;
    struct chal c, best;
    int rank = 0, kind, alg;
    struct out o;
    o.p = out, o.at = 0, o.room = room;
    memset(&best, 0, sizeof best);
    while (challenge(&p, end, &c)) {
        int r = 0;
        if (same(c.scheme, c.sl, "Basic"))
            r = 1;
        else if (same(c.scheme, c.sl, "Digest") && c.nonce && (alg = digest_alg(&c)) != 0
                 && (!c.qop || has_item(c.qop, c.ql, "auth")))
            r = (alg & 2) ? 3 : 2;
        if (r > rank)
            rank = r, best = c;
    }
    if (!rank || !room)
        return 0;
    if (rank == 1) {
        /* Basic: base64 of user:password, done in pieces of 3 bytes. */
        unsigned char tri[3];
        char quad[5];
        unsigned long ul = strlen(user), pl = strlen(password), n = ul + 1 + pl, i;
        int k = 0;
        puts_(&o, "Basic ");
        for (i = 0; i < n; i++) {
            tri[k++] = (unsigned char)(i < ul ? user[i] : i == ul ? ':' : password[i - ul - 1]);
            if (k == 3 || i + 1 == n) {
                wfn_base64(tri, (unsigned long)k, quad);
                puts_(&o, quad);
                k = 0;
            }
        }
    } else {
        unsigned char d[32];
        char ha1[65], ha2[65], resp[65], ncs[9], uh[65];
        struct hctx h;
        int i, hl;
        alg = digest_alg(&best);
        kind = (alg & 2) ? H_SHA256 : H_MD5;
        hinit(&h, kind);
        hcolon(&h, user, strlen(user));
        hcolon(&h, best.realm ? best.realm : "", best.rl);
        hstr(&h, password);
        hl = hfinal(&h, d);
        hex(d, hl, ha1);
        if (alg & 4) {
            hinit(&h, kind);
            hcolon(&h, ha1, (unsigned long)hl * 2);
            hcolon(&h, best.nonce, best.nl);
            hstr(&h, cnonce);
            hex(d, hfinal(&h, d), ha1);
        }
        hinit(&h, kind);
        hcolon(&h, method, strlen(method));
        hstr(&h, uri);
        hex(d, hfinal(&h, d), ha2);
        for (i = 0; i < 8; i++)
            ncs[i] = "0123456789abcdef"[(nc >> (28 - 4 * i)) & 15];
        ncs[8] = 0;
        hinit(&h, kind);
        hcolon(&h, ha1, (unsigned long)hl * 2);
        hcolon(&h, best.nonce, best.nl);
        if (best.qop) {
            hcolon(&h, ncs, 8);
            hcolon(&h, cnonce, strlen(cnonce));
            hcolon(&h, "auth", 4);
        }
        hstr(&h, ha2);
        hex(d, hfinal(&h, d), resp);
        puts_(&o, "Digest username=\"");
        if (best.userhash && same(best.userhash, best.ul, "true")) {
            hinit(&h, kind);
            hcolon(&h, user, strlen(user));
            hupdate(&h, best.realm ? best.realm : "", best.rl);
            hex(d, hfinal(&h, d), uh);
            puts_(&o, uh);
        } else
            puts_(&o, user);
        puts_(&o, "\", realm=\"");
        put(&o, best.realm ? best.realm : "", best.rl);
        puts_(&o, "\", nonce=\"");
        put(&o, best.nonce, best.nl);
        puts_(&o, "\", uri=\"");
        puts_(&o, uri);
        puts_(&o, "\", algorithm=");
        puts_(&o, alg == 1 ? "MD5" : alg == 5 ? "MD5-sess" : alg == 2 ? "SHA-256" : "SHA-256-sess");
        if (best.qop) {
            puts_(&o, ", qop=auth, nc=");
            puts_(&o, ncs);
            puts_(&o, ", cnonce=\"");
            puts_(&o, cnonce);
            puts_(&o, "\"");
        }
        puts_(&o, ", response=\"");
        puts_(&o, resp);
        puts_(&o, "\"");
        if (best.opaque) {
            puts_(&o, ", opaque=\"");
            put(&o, best.opaque, best.ol);
            puts_(&o, "\"");
        }
        if (best.userhash && same(best.userhash, best.ul, "true"))
            puts_(&o, ", userhash=true");
    }
    if (o.at >= room)
        return 0;
    out[o.at] = 0;
    return o.at;
}
