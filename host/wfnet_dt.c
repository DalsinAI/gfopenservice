/*
 * The datatypes calls of src/wfnet_dec.h on Linux, for host/test_wfnet.py:
 * stream.datatype's portable core (opendatatypes Datatypes/stream/sdtcore.c)
 * with gzip.datatype's decoder (gzip/gzcore.c, over z.library's calls,
 * tests/zlhost.c) and brotli.datatype's (brotli/brdecode.c, with
 * WITH_BROTLI), standing in for datatypes.library's NewDTObject() and the
 * class's SDTM_FEED, SDTM_TAKE and SDTM_END, as streamclass.c answers them.
 * WFNP_DT_MISSING ("brotli", "gzip brotli") names datatypes to act as
 * if not installed.
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "sdtcore.h"
#include "gzcore.h"
#ifdef WITH_BROTLI
#include "brdecode.h"
#endif
#include "wfnet_dec.h"

struct obj {
    struct sdt_core core;
    struct sdt_env env;
    struct gz_state gz;
#ifdef WITH_BROTLI
    struct br_state *br;
#endif
};

static long live;                   /* objects not yet disposed, for the test */

long wfnp_dt_live(void)
{
    return live;
}

static void *e_alloc(struct sdt_env *e, ULONG n)
{
    (void)e;
    return malloc(n ? n : 1);
}

static void e_free(struct sdt_env *e, void *p)
{
    (void)e;
    free(p);
}

static LONG codec(void *ctx, struct sdt_call *c)
{
    struct obj *o = ctx;
#ifdef WITH_BROTLI
    if (o->br)
        return br_run(o->br, c);
#endif
    return gz_run(&o->gz, c);
}

void *wfn_dt_alloc(void *env, unsigned long size)
{
    (void)env;
    return calloc(1, size);
}

void wfn_dt_release(void *env, void *p)
{
    (void)env;
    free(p);
}

void *wfn_dt_new(void *env, const char *name, unsigned long wrapper)
{
    const char *missing = getenv("WFNP_DT_MISSING");
    struct obj *o;
    (void)env;
    if ((missing && strstr(missing, name)) || !(o = calloc(1, sizeof *o)))
        return NULL;
    o->env.alloc = e_alloc;
    o->env.free = e_free;
    if (!strcmp(name, "gzip"))
        gz_init(&o->gz, (struct Library *)o, (LONG)wrapper);   /* zlhost.c needs no base */
#ifdef WITH_BROTLI
    else if (!strcmp(name, "brotli") && (o->br = br_new(&o->env)) != NULL)
        ;
#endif
    else {
        free(o);
        return NULL;
    }
    sdt_init(&o->core, &o->env, codec, o);
    live++;
    return o;
}

int wfn_dt_feed(void *env, void *o, const unsigned char *p, unsigned long n)
{
    (void)env;
    return !sdt_feed(&((struct obj *)o)->core, p, (ULONG)n);
}

unsigned long wfn_dt_take(void *env, void *o, unsigned char *p, unsigned long n, unsigned long *flags)
{
    ULONG f, got;
    (void)env;
    got = sdt_take(&((struct obj *)o)->core, p, (ULONG)n, &f);
    *flags = f;                     /* SDT_F_ are SDTF_, and WFN_DT_ */
    return got;
}

void wfn_dt_end(void *env, void *o)
{
    (void)env;
    sdt_end(&((struct obj *)o)->core);
}

void wfn_dt_dispose(void *env, void *p)
{
    struct obj *o = p;
    (void)env;
#ifdef WITH_BROTLI
    if (o->br)
        br_free(o->br);
    else
#endif
        gz_free(&o->gz);
    sdt_free(&o->core);
    free(o);
    live--;
}
