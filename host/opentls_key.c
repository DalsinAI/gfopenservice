/*
 * opentls.key/1 on the host, with OpenSSL 3 (opentls_key.h).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <string.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include "opentls_key.h"

/* Properties for every fetch: NULL on the host; "provider=default" when an
 * Amiga does the work itself beside the provider that sends it away. */
const char *otk_propq;

static const char *digestFor(uint32_t id)
{
    switch (id) {
    case OTK_SHA1: return "SHA1";
    case OTK_SHA256: return "SHA256";
    case OTK_SHA384: return "SHA384";
    case OTK_SHA512: return "SHA512";
    }
    return NULL;
}

static int verify(uint32_t alg, const uint32_t extra[4], struct otk_buffer buf[4], uint32_t *result)
{
    const unsigned char *p = buf[0].in;
    EVP_PKEY *key;
    int ok, status = OTK_OK;
    if (!buf[0].in || !buf[1].in || !buf[2].in)
        return OTK_BADREQUEST;
    key = d2i_PUBKEY_ex(NULL, &p, buf[0].length, NULL, otk_propq);
    if (!key)
        return OTK_BADREQUEST;
    if (alg == OTK_ED25519) {
        EVP_MD_CTX *md = EVP_MD_CTX_new();
        ok = md && EVP_DigestVerifyInit_ex(md, NULL, NULL, NULL, otk_propq, key, NULL) == 1
             && EVP_DigestVerify(md, buf[2].in, buf[2].length, buf[1].in, buf[1].length) == 1;
        EVP_MD_CTX_free(md);
    } else {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, key, otk_propq);
        const char *md = digestFor(extra[0]);
        EVP_MD *digest = md ? EVP_MD_fetch(NULL, md, otk_propq) : NULL;
        if (!ctx || !digest || EVP_PKEY_verify_init(ctx) <= 0 || EVP_PKEY_CTX_set_signature_md(ctx, digest) <= 0)
            status = OTK_BADREQUEST;
        else if (alg == OTK_RSA_PKCS1 && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0)
            status = OTK_BADREQUEST;
        else if (alg == OTK_RSA_PSS
                 && (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING) <= 0
                     || EVP_PKEY_CTX_set_rsa_mgf1_md_name(ctx, md, NULL) <= 0
                     || EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, extra[1] ? (int)extra[1] : RSA_PSS_SALTLEN_DIGEST) <= 0))
            status = OTK_BADREQUEST;
        else if (alg != OTK_RSA_PKCS1 && alg != OTK_RSA_PSS && alg != OTK_ECDSA)
            status = OTK_BADREQUEST;
        ok = status == OTK_OK && EVP_PKEY_verify(ctx, buf[2].in, buf[2].length, buf[1].in, buf[1].length) == 1;
        EVP_PKEY_CTX_free(ctx);
        EVP_MD_free(digest);
    }
    EVP_PKEY_free(key);
    *result = ok ? 1 : 0;
    return status;
}

static const char *groupFor(uint32_t curve, uint32_t *privLength)
{
    switch (curve) {
    case OTK_P256: *privLength = 32; return "P-256";
    case OTK_P384: *privLength = 48; return "P-384";
    }
    return NULL;
}

/* A new key of type, on group (EC) or none (X25519). EVP_PKEY_Q_keygen's
 * variable arguments do not survive AmiSSL's macros, so step by step. */
static EVP_PKEY *newKey(const char *type, const char *group)
{
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, type, otk_propq);
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || (group && EVP_PKEY_CTX_set_group_name(ctx, group) <= 0)
        || EVP_PKEY_keygen(ctx, &key) <= 0)
        key = NULL;
    EVP_PKEY_CTX_free(ctx);
    return key;
}

static int put(struct otk_buffer *b, const unsigned char *data, size_t length)
{
    if (!b->out || b->length < length)
        return 0;
    memcpy(b->out, data, length);
    b->written = (uint32_t)length;
    return 1;
}

static int keygen(uint32_t curve, struct otk_buffer buf[4], uint32_t *result, uint32_t *aux)
{
    unsigned char priv[66], pub[133];
    size_t privLength = sizeof priv, pubLength = sizeof pub;
    EVP_PKEY *key = NULL;
    int status = OTK_HOSTERROR;
    if (curve == OTK_X25519) {
        key = newKey("X25519", NULL);
        if (key && EVP_PKEY_get_raw_private_key(key, priv, &privLength) == 1
            && EVP_PKEY_get_raw_public_key(key, pub, &pubLength) == 1)
            status = OTK_OK;
    } else {
        uint32_t want;
        const char *group = groupFor(curve, &want);
        BIGNUM *d = NULL;
        if (!group)
            return OTK_BADREQUEST;
        key = newKey("EC", group);
        if (key && EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &d) == 1
            && BN_bn2binpad(d, priv, (int)want) == (int)want
            && EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, pub, sizeof pub, &pubLength) == 1) {
            privLength = want;
            status = OTK_OK;
        }
        BN_clear_free(d);
    }
    EVP_PKEY_free(key);
    if (status == OTK_OK && (!put(&buf[0], priv, privLength) || !put(&buf[1], pub, pubLength)))
        status = OTK_TOOSMALL;
    OPENSSL_cleanse(priv, sizeof priv);
    *result = (uint32_t)pubLength;
    *aux = (uint32_t)privLength;
    return status;
}

/* An EC key from its curve and either the private scalar or the public point. */
static EVP_PKEY *ecKey(const char *group, const unsigned char *priv, size_t privLength,
                       const unsigned char *pub, size_t pubLength)
{
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", otk_propq);
    EVP_PKEY *key = NULL;
    BIGNUM *d = priv ? BN_bin2bn(priv, (int)privLength, NULL) : NULL;
    if (bld && ctx && OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, group, 0)
        && (!d || OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, d))
        && (!pub || OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub, pubLength))
        && (params = OSSL_PARAM_BLD_to_param(bld))
        && EVP_PKEY_fromdata_init(ctx) > 0)
        EVP_PKEY_fromdata(ctx, &key, priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(ctx);
    BN_clear_free(d);
    return key;
}

static int derive(uint32_t curve, struct otk_buffer buf[4], uint32_t *result)
{
    EVP_PKEY *ours = NULL, *theirs = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    unsigned char secret[66];
    size_t length = sizeof secret;
    int status = OTK_BADREQUEST;
    if (!buf[0].in || !buf[1].in)
        return OTK_BADREQUEST;
    if (curve == OTK_X25519) {
        if (buf[0].length == 32 && buf[1].length == 32) {
            ours = EVP_PKEY_new_raw_private_key_ex(NULL, "X25519", otk_propq, buf[0].in, 32);
            theirs = EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", otk_propq, buf[1].in, 32);
        }
    } else {
        uint32_t want;
        const char *group = groupFor(curve, &want);
        if (group && buf[0].length == want) {
            ours = ecKey(group, buf[0].in, buf[0].length, NULL, 0);
            theirs = ecKey(group, NULL, 0, buf[1].in, buf[1].length);
        }
    }
    if (ours && theirs) {
        ctx = EVP_PKEY_CTX_new_from_pkey(NULL, ours, otk_propq);
        status = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, theirs) > 0
                 && EVP_PKEY_derive(ctx, secret, &length) > 0 ? OTK_OK : OTK_BADREQUEST;
    }
    if (status == OTK_OK && !put(&buf[2], secret, length))
        status = OTK_TOOSMALL;
    OPENSSL_cleanse(secret, sizeof secret);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(ours);
    EVP_PKEY_free(theirs);
    *result = status == OTK_OK ? (uint32_t)length : 0;
    return status;
}

int otk_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct otk_buffer buf[4],
             uint32_t *result, uint32_t *aux)
{
    int i;
    *result = *aux = 0;
    for (i = 0; i < 4; i++)
        buf[i].written = 0;
    switch (op) {
    case OTK_VERIFY: return verify(arg, extra, buf, result);
    case OTK_KEYGEN: return keygen(arg, buf, result, aux);
    case OTK_DERIVE: return derive(arg, buf, result);
    }
    return OTK_BADREQUEST;
}
