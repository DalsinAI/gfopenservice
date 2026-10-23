/*
 * wfnet's decoders (wfnet_dec.h): Content-Encoding into the datatypes'
 * streaming objects, and the coded bytes through them.
 *
 * One object per coding: o[0] is fed the body as it comes, and each takes
 * from the one before it as it gets hungry, through a small buffer that
 * the next one copies at once (SDTM_FEED copies). Coded bytes are fed only
 * when the plain side asks for more, so what waits in the objects stays
 * about one piece of the body.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#include "wfnet_dec.h"

#define STAGES 3                /* codings undone one after another, at most */
#define TMP    2048             /* plain bytes of one going to the next */

enum { K_GZIP = 1, K_DEFLATE, K_BR };

struct dec {
    void *o[STAGES];            /* o[0] takes the coded bytes, o[n - 1] gives the plain ones */
    unsigned char n;
    unsigned char ended;        /* bit i: SDTM_END sent to o[i] */
    unsigned char fed, done;
    char name[STAGES * 17 + 1]; /* "brotli.datatype, gzip.datatype" */
    unsigned char tmp[TMP];
};

/* s[0..n) is lit (lower case), in any case. */
static int ieq(const char *s, unsigned long n, const char *lit)
{
    for (; n && *lit; s++, lit++, n--)
        if ((*s >= 'A' && *s <= 'Z' ? *s + 32 : *s) != *lit)
            return 0;
    return !n && !*lit;
}

static unsigned long add(char *d, unsigned long at, const char *s)
{
    while (*s)
        d[at++] = *s++;
    d[at] = 0;
    return at;
}

void *wfn_dec_new(void *env, const char *coding)
{
    unsigned char kind[STAGES];
    const char *s;
    struct dec *d;
    unsigned long n, at = 0;
    int k = 0, i;
    while (*coding) {
        while (*coding == ' ' || *coding == '\t' || *coding == ',')
            coding++;
        for (s = coding; *coding && *coding != ' ' && *coding != '\t' && *coding != ','; coding++)
            ;
        if (!(n = (unsigned long)(coding - s)) || ieq(s, n, "identity"))
            continue;
        if (k == STAGES)
            return 0;
        kind[k] = ieq(s, n, "gzip") || ieq(s, n, "x-gzip") ? K_GZIP : ieq(s, n, "deflate") ? K_DEFLATE
                : ieq(s, n, "br") ? K_BR : 0;
        if (!kind[k++])
            return 0;
    }
    if (!k || !(d = wfn_dt_alloc(env, sizeof *d)))
        return 0;
    for (i = 0; i < k; i++) {
        int c = kind[k - 1 - i];            /* the last applied is undone first */
        const char *name = c == K_BR ? "brotli" : "gzip";
        if (!(d->o[i] = wfn_dt_new(env, name, c == K_GZIP ? WFN_DTW_GZIP : WFN_DTW_AUTO))) {
            wfn_dec_free(env, d);
            return 0;
        }
        d->n++;
        if (i)
            at = add(d->name, at, ", ");
        at = add(d->name, add(d->name, at, name), ".datatype");
    }
    return d;
}

void wfn_dec_free(void *env, void *dec)
{
    struct dec *d = dec;
    while (d->n)
        wfn_dt_dispose(env, d->o[--d->n]);
    wfn_dt_release(env, d);
}

const char *wfn_dec_name(void *env, void *dec)
{
    (void)env;
    return ((struct dec *)dec)->name;
}

/* Plain bytes from o[i] into p, moving the ones before it on as it needs:
 * how many, and *flags as SDTM_TAKE's. */
static unsigned long pull(void *env, struct dec *d, int i, unsigned char *p, unsigned long room, unsigned long *flags)
{
    for (;;) {
        unsigned long got = wfn_dt_take(env, d->o[i], p, room, flags), f, m;
        if (got || !i || !(*flags & WFN_DT_HUNGRY))
            return got;
        m = pull(env, d, i - 1, d->tmp, TMP, &f);
        if (m) {
            if (!wfn_dt_feed(env, d->o[i], d->tmp, m))
                break;
        } else if ((f & WFN_DT_END) && !(d->ended & 1U << i)) {
            d->ended |= (unsigned char)(1U << i);
            wfn_dt_end(env, d->o[i]);
        } else if (f & WFN_DT_HUNGRY)
            return 0;                       /* *flags still says hungry */
        else
            break;
    }
    *flags = WFN_DT_ERROR;
    return 0;
}

long wfn_dec_run(void *env, void *dec, const unsigned char *in, unsigned long length, unsigned long *used,
                 unsigned char *out, unsigned long room, int last)
{
    struct dec *d = dec;
    unsigned long made = 0, f;
    *used = 0;
    if (last && !d->fed && !length)
        d->done = 1;                        /* no body at all (HEAD, 304): nothing to undo */
    while (!d->done && made < room) {
        made += pull(env, d, d->n - 1, out + made, room - made, &f);
        if (f & WFN_DT_ERROR)
            return -1;
        if (f & WFN_DT_END)
            d->done = 1;
        else if (made == room)
            break;
        else if (*used < length) {
            if (!wfn_dt_feed(env, d->o[0], in, length))
                return -1;
            *used = length;
            d->fed = 1;
        } else if (last && !(d->ended & 1)) {
            d->ended |= 1;
            wfn_dt_end(env, d->o[0]);
        } else
            break;
    }
    if (d->done)
        *used = length;                     /* bytes after the stream's end are not the body's */
    return (long)made;
}

const char *wfn_dec_accept(void *env, unsigned char *seen)
{
    void *o;
    if (!*seen) {
        *seen = 4;
        if ((o = wfn_dt_new(env, "gzip", WFN_DTW_AUTO)) != 0) {
            wfn_dt_dispose(env, o);
            *seen |= 1;
        }
        if ((o = wfn_dt_new(env, "brotli", WFN_DTW_AUTO)) != 0) {
            wfn_dt_dispose(env, o);
            *seen |= 2;
        }
    }
    return (*seen & 3) == 3 ? "gzip, deflate, br" : *seen & 1 ? "gzip, deflate" : *seen & 2 ? "br" : 0;
}
