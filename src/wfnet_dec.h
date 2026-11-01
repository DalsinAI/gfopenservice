/*
 * wfnet's decoders (src/wfnet_dec.c): a body's Content-Encoding undone by
 * the datatypes, stream.datatype's gzip.datatype (gzip, and "deflate"
 * zlib-wrapped or raw) and brotli.datatype, through struct wfn_io's dec_
 * hooks. No compression code of our own here: this only reads the header,
 * makes the datatypes' streaming objects and moves bytes through them.
 *
 * The machine gives the datatypes calls (wfn_dt_*): src/wfnet_amiga.c with
 * datatypes.library, host/wfnet_dt.c with stream.datatype's portable core
 * for the tests. env is struct wfn_io's.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#ifndef WFNET_DEC_H
#define WFNET_DEC_H

/* SDTM_TAKE's sdtt_Flags and SDTA_Wrapper's values (datatypes/streamclass.h) */
#define WFN_DT_END    1UL       /* SDTF_END */
#define WFN_DT_HUNGRY 2UL       /* SDTF_HUNGRY */
#define WFN_DT_ERROR  4UL       /* SDTF_ERROR */
#define WFN_DTW_AUTO  0UL       /* SDTW_AUTO: gzip or zlib by the first bytes, else raw deflate */
#define WFN_DTW_GZIP  1UL       /* SDTW_GZIP */

/* The machine's side. */
void *wfn_dt_alloc(void *env, unsigned long size);      /* zeroed, or NULL */
void wfn_dt_release(void *env, void *p);
/* A streaming object (DTST_RAM) of the datatype named ("gzip", "brotli"),
 * or NULL when that datatype, or datatypes.library, is not there. */
void *wfn_dt_new(void *env, const char *name, unsigned long wrapper);
int wfn_dt_feed(void *env, void *o, const unsigned char *p, unsigned long n);  /* SDTM_FEED: 1, or 0 */
/* SDTM_TAKE: the count, and *flags (WFN_DT_*). */
unsigned long wfn_dt_take(void *env, void *o, unsigned char *p, unsigned long n, unsigned long *flags);
void wfn_dt_end(void *env, void *o);                    /* SDTM_END */
void wfn_dt_dispose(void *env, void *o);                /* DisposeDTObject() */

/* struct wfn_io's dec_new, dec_run, dec_free and dec_name, as they are.
 * Several codings ("gzip, br") are undone one after another, the last
 * applied first, up to three; a coding with no datatype for it, or more
 * than three, and dec_new answers NULL (the body then comes as sent). */
void *wfn_dec_new(void *env, const char *coding);
long wfn_dec_run(void *env, void *dec, const unsigned char *in, unsigned long length, unsigned long *used,
                 unsigned char *out, unsigned long room, int last);
void wfn_dec_free(void *env, void *dec);
const char *wfn_dec_name(void *env, void *dec);
/* For dec_accept: the codings whose datatypes are there ("gzip, deflate,
 * br", "gzip, deflate", "br"), or NULL for none. They are looked for once,
 * and *seen (0 at first, kept by the machine) remembers what was found. */
const char *wfn_dec_accept(void *env, unsigned char *seen);

#endif
