/*
 * opentls: TLS public-key work at pace. A program using OpenSSL 3 (AmiSSL
 * on the Amiga) calls opentls_install() once; from then on the key maths of
 * every TLS handshake (certificate and handshake signatures, the key
 * exchange) goes to the transport, normally gfopenservice.device's
 * opentls.key/1, and is done here when the transport cannot.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OPENTLS_H
#define OPENTLS_H

#include <stdint.h>
#include "opentls_key.h"

/* Does one opentls.key/1 request elsewhere; returns OTK_OK, or anything
 * else to have it done here. */
typedef int (*opentls_transport)(uint16_t op, uint32_t arg, const uint32_t extra[4],
                                 struct otk_buffer buf[4], uint32_t *result, uint32_t *aux);

/* Registers the "obkey" provider and prefers it. With t NULL every
 * operation is done here. Returns 0 when OpenSSL would not take it. */
int opentls_install(opentls_transport t);

struct opentls_stats {
    unsigned long verifies, keygens, derives;   /* asked for */
    unsigned long sent, local;                  /* where they ran */
};
extern struct opentls_stats opentls_stats;

#endif
