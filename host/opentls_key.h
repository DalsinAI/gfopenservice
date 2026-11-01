/*
 * opentls.key/1: TLS public-key work for an Amiga, on the host
 * (docs/OPENTLS_KEY.md). One call per request; no state between calls.
 * The emulator's service host and the LAN Cradle both call this.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OPENTLS_KEY_H
#define OPENTLS_KEY_H

#include <stdint.h>

#define OTK_VERIFY 1   /* buf0 key (SubjectPublicKeyInfo DER), buf1 digest, buf2 signature -> result 1 good, 0 bad */
#define OTK_KEYGEN 2   /* arg curve -> buf0 private, buf1 public; result: public length, aux: private length */
#define OTK_DERIVE 3   /* arg curve, buf0 private, buf1 peer public -> buf2 secret; result: its length */

/* OTK_VERIFY: arg */
#define OTK_RSA_PKCS1 1
#define OTK_RSA_PSS   2
#define OTK_ECDSA     3
#define OTK_ED25519   4   /* buf1 is the whole message, not a digest */

/* OTK_VERIFY: extra[0], the digest buf1 holds */
#define OTK_SHA1   1
#define OTK_SHA256 2
#define OTK_SHA384 3
#define OTK_SHA512 4

/* OTK_KEYGEN, OTK_DERIVE: arg */
#define OTK_X25519 1
#define OTK_P256   2
#define OTK_P384   3

/* Status, as the services card's. */
#define OTK_OK          0
#define OTK_BADREQUEST (-2)
#define OTK_TOOSMALL   (-4)
#define OTK_HOSTERROR  (-5)

struct otk_buffer {
    const uint8_t *in;     /* the Amiga's bytes, for a buffer the service reads */
    uint8_t *out;          /* where to write, for a buffer the service writes */
    uint32_t length;       /* in: its length; out: the room */
    uint32_t written;      /* out: bytes written */
};

/* Properties for OpenSSL fetches (default NULL). */
extern const char *otk_propq;

/* Runs one request. Returns the status; sets *result and *aux. */
int otk_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct otk_buffer buf[4],
             uint32_t *result, uint32_t *aux);

#endif
