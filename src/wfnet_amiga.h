/*
 * wfnet on the Amiga (src/wfnet_amiga.c): the native transport of
 * openweb.fetch over bsdsocket.library and gfopentls.library, for
 * src/webfetch.c. wfna_new answers NULL when no TCP/IP stack is running.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 * SPDX-License-Identifier: MIT
 */
#ifndef WFNET_AMIGA_H
#define WFNET_AMIGA_H

#include <exec/types.h>
#include "openweb/wfnet.h"

struct wfna;

struct wfna *wfna_new(unsigned long window);
void wfna_free(struct wfna *e);
/* One op of openweb.fetch, as the service answers it. */
LONG wfna_call(struct wfna *e, UWORD op, ULONG arg, const ULONG extra[4], struct wfn_buf buf[4], ULONG *result, ULONG *aux);
/* Moves every fetch on, waiting at most ms for the network or one of sigs:
 * which of sigs came (0 when none did). Ctrl-C ends the wait too, and is
 * left set for the program. */
ULONG wfna_wait(struct wfna *e, ULONG ms, ULONG sigs);

#endif
