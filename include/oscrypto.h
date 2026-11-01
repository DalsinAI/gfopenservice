/*
 * The Nursery LAN link's cryptography (oscrypto.c).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OSCRYPTO_H
#define OSCRYPTO_H

#include <stddef.h>
#include <stdint.h>

struct os_sha256 {
    uint32_t h[8];
    uint32_t bytes;
    uint8_t buf[64];
    size_t used;
};

void os_sha256_init(struct os_sha256 *);
void os_sha256_update(struct os_sha256 *, const void *data, size_t length);
void os_sha256_final(struct os_sha256 *, uint8_t out[32]);
void os_sha256(const void *data, size_t length, uint8_t out[32]);
void os_hmac_sha256(const uint8_t *key, size_t keyLength, const void *data, size_t length, uint8_t out[32]);
void os_hkdf_sha256(const uint8_t *secret, size_t secretLength, const uint8_t *salt, size_t saltLength,
                    const char *info, uint8_t *out, size_t outLength);

/* ChaCha20-Poly1305 in place: seal encrypts data and writes the tag; open
 * checks the tag, then decrypts, and returns 0 (data untouched) when the
 * tag is wrong. */
void os_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadLength,
                  uint8_t *data, size_t length, uint8_t tag[16]);
int os_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadLength,
                 uint8_t *data, size_t length, const uint8_t tag[16]);

void os_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void os_x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#endif
