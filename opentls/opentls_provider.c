/*
 * opentls: an OpenSSL 3 provider, "obkey", that sends TLS public-key work
 * to the opentls.key/1 service (opentls.h). Bulk encryption, hashing and
 * everything else stay with the default provider.
 *
 * It offers key management, signature verification and key exchange for the
 * key types TLS uses: RSA and EC public keys (certificates and the server's
 * handshake signature), and X25519 and EC key shares. Each operation goes to
 * the transport; when that fails, the same work is done here with the
 * default provider (host/opentls_key.c, with otk_propq "provider=default").
 *
 * It never calls the core functions OpenSSL hands a provider: AmiSSL's 68k
 * build is base-relative, and those pointers lead into the library without
 * its data register. Everything it needs it calls through the library.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <string.h>
#include <stdlib.h>
#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/provider.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>

#include "opentls.h"
#include "opentls_key.h"

#define PROPS "provider=obkey"
#define DEFAULT_PROPS "provider=default"

static opentls_transport transport;
struct opentls_stats opentls_stats;

/* One operation: the transport, else here. */
static int run(uint16_t op, uint32_t arg, const uint32_t extra[4], struct otk_buffer buf[4], uint32_t *result, uint32_t *aux)
{
    int status = -1;
    if (transport) {
        struct otk_buffer copy[4];
        memcpy(copy, buf, sizeof copy);
        status = transport(op, arg, extra, copy, result, aux);
        if (status == OTK_OK) {
            memcpy(buf, copy, sizeof copy);
            opentls_stats.sent++;
            return OTK_OK;
        }
    }
    opentls_stats.local++;
    return otk_call(op, arg, extra, buf, result, aux);
}

/* ---- keys --------------------------------------------------------------- */

enum { KT_RSA = 1, KT_EC, KT_X25519 };

struct okey {
    int type;
    int curve;                     /* OTK_X25519, OTK_P256, OTK_P384 */
    unsigned char *spki;           /* the public key as DER, for VERIFY */
    size_t spkiLength;
    unsigned char pub[133];        /* EC: uncompressed point; X25519: raw */
    size_t pubLength;
    unsigned char priv[66];
    size_t privLength;
    int bits, securityBits, maxSize;
    BIGNUM *n, *e;                 /* RSA */
};

static const char *curveGroup(int curve)
{
    switch (curve) {
    case OTK_P256: return "P-256";
    case OTK_P384: return "P-384";
    case OTK_X25519: return "X25519";
    }
    return NULL;
}

static int groupCurve(const char *name)
{
    if (!name)
        return 0;
    if (!strcmp(name, "P-256") || !strcmp(name, "prime256v1") || !strcmp(name, "secp256r1"))
        return OTK_P256;
    if (!strcmp(name, "P-384") || !strcmp(name, "secp384r1"))
        return OTK_P384;
    return 0;
}

static void *keyNew(int type)
{
    struct okey *k = calloc(1, sizeof *k);
    if (k)
        k->type = type;
    return k;
}
static void *rsaNew(void *provctx) { (void)provctx; return keyNew(KT_RSA); }
static void *ecNew(void *provctx) { (void)provctx; return keyNew(KT_EC); }
static void *x25519New(void *provctx)
{
    struct okey *k = keyNew(KT_X25519);
    (void)provctx;
    if (k) {
        k->curve = OTK_X25519;
        k->bits = 253;
        k->securityBits = 128;
        k->maxSize = 32;
    }
    return k;
}

static void keyFree(void *kd)
{
    struct okey *k = kd;
    if (!k)
        return;
    OPENSSL_cleanse(k->priv, sizeof k->priv);
    free(k->spki);
    BN_free(k->n);
    BN_free(k->e);
    free(k);
}

static void *keyDup(const void *kd, int selection)
{
    const struct okey *from = kd;
    struct okey *k = calloc(1, sizeof *k);
    (void)selection;
    if (!k)
        return NULL;
    *k = *from;
    k->spki = NULL;
    k->n = k->e = NULL;
    if (from->spki && (k->spki = malloc(from->spkiLength)))
        memcpy(k->spki, from->spki, from->spkiLength);
    if (from->n)
        k->n = BN_dup(from->n);
    if (from->e)
        k->e = BN_dup(from->e);
    return k;
}

static int keyHas(const void *kd, int selection)
{
    const struct okey *k = kd;
    if (!k)
        return 0;
    if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) && !k->privLength)
        return 0;
    if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) && !k->pubLength && !k->spki)
        return 0;
    if ((selection & OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS) && k->type == KT_EC && !k->curve)
        return 0;
    return 1;
}

/* A default-provider key with the same public half, for DER and export. */
static EVP_PKEY *defaultPublic(const struct okey *k)
{
    const unsigned char *p = k->spki;
    if (k->spki)
        return d2i_PUBKEY_ex(NULL, &p, (long)k->spkiLength, NULL, DEFAULT_PROPS);
    if (k->type == KT_X25519 && k->pubLength == 32)
        return EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", DEFAULT_PROPS, k->pub, 32);
    if (k->type == KT_EC && k->curve && k->pubLength) {
        OSSL_PARAM params[3];
        EVP_PKEY *pk = NULL;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", DEFAULT_PROPS);
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)curveGroup(k->curve), 0);
        params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, (void *)k->pub, k->pubLength);
        params[2] = OSSL_PARAM_construct_end();
        if (ctx && EVP_PKEY_fromdata_init(ctx) > 0)
            EVP_PKEY_fromdata(ctx, &pk, EVP_PKEY_PUBLIC_KEY, params);
        EVP_PKEY_CTX_free(ctx);
        return pk;
    }
    return NULL;
}

/* Fills spki, bits and sizes from a default-provider key. */
static int takePublic(struct okey *k, EVP_PKEY *pk)
{
    int length = i2d_PUBKEY(pk, NULL);
    unsigned char *p;
    if (length <= 0 || !(k->spki = malloc(length)))
        return 0;
    p = k->spki;
    k->spkiLength = (size_t)i2d_PUBKEY(pk, &p);
    k->bits = EVP_PKEY_get_bits(pk);
    k->securityBits = EVP_PKEY_get_security_bits(pk);
    k->maxSize = EVP_PKEY_get_size(pk);
    return 1;
}

static int keyImport(void *kd, int selection, const OSSL_PARAM params[])
{
    struct okey *k = kd;
    const OSSL_PARAM *p;
    if (k->type == KT_RSA) {
        EVP_PKEY_CTX *ctx;
        EVP_PKEY *pk = NULL;
        int ok;
        /* Public keys only (certificates). An export asks for everything,
         * so refuse only a key that really has its private half. */
        if (!(selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) || OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_D))
            return 0;
        if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_N)))
            OSSL_PARAM_get_BN(p, &k->n);
        if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_E)))
            OSSL_PARAM_get_BN(p, &k->e);
        ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", DEFAULT_PROPS);
        ok = ctx && EVP_PKEY_fromdata_init(ctx) > 0 && EVP_PKEY_fromdata(ctx, &pk, EVP_PKEY_PUBLIC_KEY, (OSSL_PARAM *)params) > 0
             && takePublic(k, pk);
        EVP_PKEY_free(pk);
        EVP_PKEY_CTX_free(ctx);
        return ok;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME))) {
        const char *name = NULL;
        if (OSSL_PARAM_get_utf8_string_ptr(p, &name) && k->type == KT_EC)
            k->curve = groupCurve(name);
    }
    if (OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PRIV_KEY))
        return 0;                     /* our private keys come from KEYGEN */
    if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY)
        && (p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PUB_KEY))) {
        const void *data;
        size_t length;
        if (!OSSL_PARAM_get_octet_string_ptr(p, &data, &length) || length > sizeof k->pub)
            return 0;
        memcpy(k->pub, data, length);
        k->pubLength = length;
    }
    if (k->type == KT_EC && k->curve) {
        EVP_PKEY *pk = k->pubLength ? defaultPublic(k) : NULL;
        k->bits = k->curve == OTK_P256 ? 256 : 384;
        k->securityBits = k->curve == OTK_P256 ? 128 : 192;
        k->maxSize = k->curve == OTK_P256 ? 72 : 104;
        if (pk && !takePublic(k, pk)) {
            EVP_PKEY_free(pk);
            return 0;
        }
        EVP_PKEY_free(pk);
    }
    return k->type != KT_EC || k->curve;
}

static const OSSL_PARAM rsaTypes[] = {
    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N, NULL, 0),
    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM ecTypes[] = {
    OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM x25519Types[] = {
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM *rsaImportTypes(int selection) { (void)selection; return rsaTypes; }
static const OSSL_PARAM *ecImportTypes(int selection) { (void)selection; return ecTypes; }
static const OSSL_PARAM *x25519ImportTypes(int selection) { (void)selection; return x25519Types; }

static int keyExport(void *kd, int selection, OSSL_CALLBACK *cb, void *cbarg)
{
    struct okey *k = kd;
    EVP_PKEY *pk;
    int ok;
    if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)
        return 0;                     /* the private half never leaves */
    pk = defaultPublic(k);
    ok = pk && EVP_PKEY_export(pk, selection & ~OSSL_KEYMGMT_SELECT_PRIVATE_KEY, cb, cbarg);
    EVP_PKEY_free(pk);
    return ok;
}

static int keyGetParams(void *kd, OSSL_PARAM params[])
{
    struct okey *k = kd;
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_BITS)) && !OSSL_PARAM_set_int(p, k->bits))
        return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_SECURITY_BITS)) && !OSSL_PARAM_set_int(p, k->securityBits))
        return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_MAX_SIZE)) && !OSSL_PARAM_set_int(p, k->maxSize))
        return 0;
    if (k->type == KT_RSA) {
        if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_RSA_N)) && (!k->n || !OSSL_PARAM_set_BN(p, k->n)))
            return 0;
        if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_RSA_E)) && (!k->e || !OSSL_PARAM_set_BN(p, k->e)))
            return 0;
        if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_DEFAULT_DIGEST)) && !OSSL_PARAM_set_utf8_string(p, "SHA256"))
            return 0;
        return 1;
    }
    if (k->type == KT_EC && (p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_GROUP_NAME))
        && !OSSL_PARAM_set_utf8_string(p, curveGroup(k->curve)))
        return 0;
    if (k->type == KT_EC && (p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_DEFAULT_DIGEST))
        && !OSSL_PARAM_set_utf8_string(p, "SHA256"))
        return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY))
        && (!k->pubLength || !OSSL_PARAM_set_octet_string(p, k->pub, k->pubLength)))
        return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PUB_KEY))
        && (!k->pubLength || !OSSL_PARAM_set_octet_string(p, k->pub, k->pubLength)))
        return 0;
    return 1;
}

static const OSSL_PARAM *keyGettableParams(void *provctx)
{
    static const OSSL_PARAM gettable[] = {
        OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, NULL),
        OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, NULL),
        OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, NULL),
        OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_DEFAULT_DIGEST, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
        OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N, NULL, 0),
        OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, NULL, 0),
        OSSL_PARAM_END
    };
    (void)provctx;
    return gettable;
}

/* The peer's key share arrives this way. */
static int keySetParams(void *kd, const OSSL_PARAM params[])
{
    struct okey *k = kd;
    const OSSL_PARAM *p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
    if (p) {
        const void *data;
        size_t length;
        if (k->type == KT_RSA || !OSSL_PARAM_get_octet_string_ptr(p, &data, &length) || length > sizeof k->pub)
            return 0;
        if (k->type == KT_X25519 && length != 32)
            return 0;
        memcpy(k->pub, data, length);
        k->pubLength = length;
        k->privLength = 0;
        free(k->spki);
        k->spki = NULL;
    }
    return 1;
}

static const OSSL_PARAM *keySettableParams(void *provctx)
{
    static const OSSL_PARAM settable[] = {
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
        OSSL_PARAM_END
    };
    (void)provctx;
    return settable;
}

static int keyMatch(const void *a, const void *b, int selection)
{
    const struct okey *x = a, *y = b;
    (void)selection;
    if (x->type != y->type || x->curve != y->curve)
        return 0;
    if (x->spki && y->spki)
        return x->spkiLength == y->spkiLength && !memcmp(x->spki, y->spki, x->spkiLength);
    return x->pubLength == y->pubLength && !memcmp(x->pub, y->pub, x->pubLength);
}

static const char *ecOperationName(int operation)
{
    return operation == OSSL_OP_KEYEXCH ? "ECDH" : operation == OSSL_OP_SIGNATURE ? "ECDSA" : NULL;
}

/* Key shares: generated by the service. */
struct gen {
    int type, curve, selection;
};

static void *genInit(int type, int curve, int selection, const OSSL_PARAM params[]);
static int genSetParams(void *g, const OSSL_PARAM params[])
{
    struct gen *gen = g;
    const OSSL_PARAM *p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME);
    if (p && gen->type == KT_EC) {
        const char *name = NULL;
        if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) || !(gen->curve = groupCurve(name)))
            return 0;
    }
    return 1;
}
static void *genInit(int type, int curve, int selection, const OSSL_PARAM params[])
{
    struct gen *gen = calloc(1, sizeof *gen);
    if (!gen)
        return NULL;
    gen->type = type;
    gen->curve = curve;
    gen->selection = selection;
    if (params && !genSetParams(gen, params)) {
        free(gen);
        return NULL;
    }
    return gen;
}
static void *ecGenInit(void *provctx, int selection, const OSSL_PARAM params[]) { (void)provctx; return genInit(KT_EC, 0, selection, params); }
static void *x25519GenInit(void *provctx, int selection, const OSSL_PARAM params[]) { (void)provctx; return genInit(KT_X25519, OTK_X25519, selection, params); }

/* TLS 1.2: the curve first as parameters, then a key from them. */
static int genSetTemplate(void *g, void *templ)
{
    struct gen *gen = g;
    struct okey *k = templ;
    if (!k || k->type != gen->type || !k->curve)
        return 0;
    gen->curve = k->curve;
    return 1;
}

static const OSSL_PARAM *genSettableParams(void *g, void *provctx)
{
    static const OSSL_PARAM settable[] = {
        OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
        OSSL_PARAM_END
    };
    (void)g;
    (void)provctx;
    return settable;
}

static void *gen(void *g, OSSL_CALLBACK *cb, void *cbarg)
{
    struct gen *gen = g;
    struct okey *k;
    struct otk_buffer buf[4];
    uint32_t extra[4] = { 0, 0, 0, 0 }, result, aux;
    (void)cb;
    (void)cbarg;
    if (!gen->curve)
        return NULL;
    k = gen->type == KT_X25519 ? x25519New(NULL) : keyNew(KT_EC);
    if (!k)
        return NULL;
    k->curve = gen->curve;
    if (!(gen->selection & OSSL_KEYMGMT_SELECT_KEYPAIR)) {   /* parameters only */
        k->bits = k->curve == OTK_P384 ? 384 : k->curve == OTK_P256 ? 256 : k->bits;
        return k;
    }
    memset(buf, 0, sizeof buf);
    buf[0].out = k->priv;
    buf[0].length = sizeof k->priv;
    buf[1].out = k->pub;
    buf[1].length = sizeof k->pub;
    opentls_stats.keygens++;
    if (run(OTK_KEYGEN, (uint32_t)k->curve, extra, buf, &result, &aux) != OTK_OK) {
        keyFree(k);
        return NULL;
    }
    k->privLength = buf[0].written;
    k->pubLength = buf[1].written;
    if (k->type == KT_EC) {
        k->bits = k->curve == OTK_P256 ? 256 : 384;
        k->securityBits = k->curve == OTK_P256 ? 128 : 192;
        k->maxSize = k->curve == OTK_P256 ? 72 : 104;
    }
    return k;
}

static void genCleanup(void *g) { free(g); }

#define KEYMGMT_COMMON(newf, importTypes) \
    { OSSL_FUNC_KEYMGMT_NEW, (void (*)(void))newf }, \
    { OSSL_FUNC_KEYMGMT_FREE, (void (*)(void))keyFree }, \
    { OSSL_FUNC_KEYMGMT_DUP, (void (*)(void))keyDup }, \
    { OSSL_FUNC_KEYMGMT_HAS, (void (*)(void))keyHas }, \
    { OSSL_FUNC_KEYMGMT_MATCH, (void (*)(void))keyMatch }, \
    { OSSL_FUNC_KEYMGMT_IMPORT, (void (*)(void))keyImport }, \
    { OSSL_FUNC_KEYMGMT_IMPORT_TYPES, (void (*)(void))importTypes }, \
    { OSSL_FUNC_KEYMGMT_EXPORT, (void (*)(void))keyExport }, \
    { OSSL_FUNC_KEYMGMT_EXPORT_TYPES, (void (*)(void))importTypes }, \
    { OSSL_FUNC_KEYMGMT_GET_PARAMS, (void (*)(void))keyGetParams }, \
    { OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS, (void (*)(void))keyGettableParams }, \
    { OSSL_FUNC_KEYMGMT_SET_PARAMS, (void (*)(void))keySetParams }, \
    { OSSL_FUNC_KEYMGMT_SETTABLE_PARAMS, (void (*)(void))keySettableParams }

static const OSSL_DISPATCH rsaKeymgmt[] = {
    KEYMGMT_COMMON(rsaNew, rsaImportTypes),
    { 0, NULL }
};
static const OSSL_DISPATCH ecKeymgmt[] = {
    KEYMGMT_COMMON(ecNew, ecImportTypes),
    { OSSL_FUNC_KEYMGMT_QUERY_OPERATION_NAME, (void (*)(void))ecOperationName },
    { OSSL_FUNC_KEYMGMT_GEN_INIT, (void (*)(void))ecGenInit },
    { OSSL_FUNC_KEYMGMT_GEN_SET_TEMPLATE, (void (*)(void))genSetTemplate },
    { OSSL_FUNC_KEYMGMT_GEN_SET_PARAMS, (void (*)(void))genSetParams },
    { OSSL_FUNC_KEYMGMT_GEN_SETTABLE_PARAMS, (void (*)(void))genSettableParams },
    { OSSL_FUNC_KEYMGMT_GEN, (void (*)(void))gen },
    { OSSL_FUNC_KEYMGMT_GEN_CLEANUP, (void (*)(void))genCleanup },
    { 0, NULL }
};
static const OSSL_DISPATCH x25519Keymgmt[] = {
    KEYMGMT_COMMON(x25519New, x25519ImportTypes),
    { OSSL_FUNC_KEYMGMT_GEN_INIT, (void (*)(void))x25519GenInit },
    { OSSL_FUNC_KEYMGMT_GEN_SET_TEMPLATE, (void (*)(void))genSetTemplate },
    { OSSL_FUNC_KEYMGMT_GEN_SET_PARAMS, (void (*)(void))genSetParams },
    { OSSL_FUNC_KEYMGMT_GEN_SETTABLE_PARAMS, (void (*)(void))genSettableParams },
    { OSSL_FUNC_KEYMGMT_GEN, (void (*)(void))gen },
    { OSSL_FUNC_KEYMGMT_GEN_CLEANUP, (void (*)(void))genCleanup },
    { 0, NULL }
};

/* ---- key exchange ---------------------------------------------------------- */

struct exch {
    struct okey *ours, *peer;
};

static void *exchNew(void *provctx) { (void)provctx; return calloc(1, sizeof(struct exch)); }
static void exchFree(void *c) { free(c); }
static void *exchDup(void *c)
{
    struct exch *d = malloc(sizeof *d);
    if (d)
        *d = *(struct exch *)c;
    return d;
}
static int exchInit(void *c, void *key, const OSSL_PARAM params[])
{
    (void)params;
    ((struct exch *)c)->ours = key;
    return key != NULL;
}
static int exchSetPeer(void *c, void *key)
{
    ((struct exch *)c)->peer = key;
    return key != NULL;
}
static int exchDerive(void *c, unsigned char *secret, size_t *length, size_t room)
{
    struct exch *x = c;
    struct otk_buffer buf[4];
    uint32_t extra[4] = { 0, 0, 0, 0 }, result, aux;
    size_t size = x->ours && x->ours->curve == OTK_P384 ? 48 : 32;
    if (!x->ours || !x->peer || !x->ours->privLength || !x->peer->pubLength)
        return 0;
    if (!secret) {
        *length = size;
        return 1;
    }
    memset(buf, 0, sizeof buf);
    buf[0].in = x->ours->priv;
    buf[0].length = (uint32_t)x->ours->privLength;
    buf[1].in = x->peer->pub;
    buf[1].length = (uint32_t)x->peer->pubLength;
    buf[2].out = secret;
    buf[2].length = (uint32_t)room;
    opentls_stats.derives++;
    if (run(OTK_DERIVE, (uint32_t)x->ours->curve, extra, buf, &result, &aux) != OTK_OK)
        return 0;
    *length = buf[2].written;
    return 1;
}

static const OSSL_DISPATCH keyexch[] = {
    { OSSL_FUNC_KEYEXCH_NEWCTX, (void (*)(void))exchNew },
    { OSSL_FUNC_KEYEXCH_FREECTX, (void (*)(void))exchFree },
    { OSSL_FUNC_KEYEXCH_DUPCTX, (void (*)(void))exchDup },
    { OSSL_FUNC_KEYEXCH_INIT, (void (*)(void))exchInit },
    { OSSL_FUNC_KEYEXCH_SET_PEER, (void (*)(void))exchSetPeer },
    { OSSL_FUNC_KEYEXCH_DERIVE, (void (*)(void))exchDerive },
    { 0, NULL }
};

/* ---- signature verification ------------------------------------------------------ */

struct sig {
    struct okey *key;
    int pad;                         /* OTK_RSA_PKCS1 or OTK_RSA_PSS for RSA */
    int saltLength;                  /* 0: the digest's */
    char mdName[32];
    EVP_MD_CTX *md;                  /* for DigestVerify */
};

static void *sigNew(void *provctx, const char *propq)
{
    struct sig *s = calloc(1, sizeof *s);
    (void)provctx;
    (void)propq;
    if (s) {
        s->pad = OTK_RSA_PKCS1;
        strcpy(s->mdName, "SHA256");
    }
    return s;
}
static void sigFree(void *c)
{
    struct sig *s = c;
    if (s)
        EVP_MD_CTX_free(s->md);
    free(s);
}
static void *sigDup(void *c)
{
    struct sig *s = c, *d = malloc(sizeof *d);
    if (!d)
        return NULL;
    *d = *s;
    d->md = NULL;
    if (s->md && (!(d->md = EVP_MD_CTX_new()) || !EVP_MD_CTX_copy_ex(d->md, s->md))) {
        sigFree(d);
        return NULL;
    }
    return d;
}

static int mdId(const char *name)
{
    if (!strcmp(name, "SHA256") || !strcmp(name, "SHA2-256") || !strcmp(name, "sha256"))
        return OTK_SHA256;
    if (!strcmp(name, "SHA384") || !strcmp(name, "SHA2-384") || !strcmp(name, "sha384"))
        return OTK_SHA384;
    if (!strcmp(name, "SHA512") || !strcmp(name, "SHA2-512") || !strcmp(name, "sha512"))
        return OTK_SHA512;
    if (!strcmp(name, "SHA1") || !strcmp(name, "SHA-1") || !strcmp(name, "sha1"))
        return OTK_SHA1;
    return 0;
}

static int sigSetParams(void *c, const OSSL_PARAM params[])
{
    struct sig *s = c;
    const OSSL_PARAM *p;
    if (!params)
        return 1;
    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST))) {
        const char *name = NULL;
        if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) || strlen(name) >= sizeof s->mdName)
            return 0;
        strcpy(s->mdName, name);
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PAD_MODE))) {
        int mode = 0;
        const char *name = NULL;
        if (p->data_type == OSSL_PARAM_UTF8_STRING && OSSL_PARAM_get_utf8_string_ptr(p, &name))
            mode = !strcmp(name, "pss") ? RSA_PKCS1_PSS_PADDING : !strcmp(name, "pkcs1") ? RSA_PKCS1_PADDING : 0;
        else
            OSSL_PARAM_get_int(p, &mode);
        if (mode == RSA_PKCS1_PSS_PADDING)
            s->pad = OTK_RSA_PSS;
        else if (mode == RSA_PKCS1_PADDING)
            s->pad = OTK_RSA_PKCS1;
        else
            return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PSS_SALTLEN))) {
        int length = 0;
        const char *name = NULL;
        if (p->data_type == OSSL_PARAM_UTF8_STRING && OSSL_PARAM_get_utf8_string_ptr(p, &name))
            length = !strcmp(name, "digest") ? 0 : atoi(name);
        else
            OSSL_PARAM_get_int(p, &length);
        s->saltLength = length > 0 ? length : 0;   /* -1 (digest) and the auto forms: the digest's */
    }
    return 1;
}

static const OSSL_PARAM *sigSettableParams(void *c, void *provctx)
{
    static const OSSL_PARAM settable[] = {
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0),
        OSSL_PARAM_END
    };
    (void)c;
    (void)provctx;
    return settable;
}

static int sigVerifyInit(void *c, void *key, const OSSL_PARAM params[])
{
    struct sig *s = c;
    s->key = key;
    return key && sigSetParams(c, params);
}

static int sigVerify(void *c, const unsigned char *signature, size_t signatureLength, const unsigned char *digest, size_t digestLength)
{
    struct sig *s = c;
    struct otk_buffer buf[4];
    uint32_t extra[4] = { 0, 0, 0, 0 }, result = 0, aux;
    if (!s->key || !s->key->spki)
        return 0;
    extra[0] = (uint32_t)mdId(s->mdName);
    extra[1] = (uint32_t)s->saltLength;
    memset(buf, 0, sizeof buf);
    buf[0].in = s->key->spki;
    buf[0].length = (uint32_t)s->key->spkiLength;
    buf[1].in = digest;
    buf[1].length = (uint32_t)digestLength;
    buf[2].in = signature;
    buf[2].length = (uint32_t)signatureLength;
    opentls_stats.verifies++;
    if (run(OTK_VERIFY, s->key->type == KT_EC ? OTK_ECDSA : (uint32_t)s->pad, extra, buf, &result, &aux) != OTK_OK)
        return 0;
    return result == 1;
}

static int sigDigestVerifyInit(void *c, const char *mdName, void *key, const OSSL_PARAM params[])
{
    struct sig *s = c;
    EVP_MD *md;
    int ok;
    if (mdName && *mdName) {
        if (strlen(mdName) >= sizeof s->mdName)
            return 0;
        strcpy(s->mdName, mdName);
    }
    if (!sigVerifyInit(c, key, params))
        return 0;
    md = EVP_MD_fetch(NULL, s->mdName, DEFAULT_PROPS);
    if (!s->md)
        s->md = EVP_MD_CTX_new();
    ok = md && s->md && EVP_DigestInit_ex2(s->md, md, NULL);
    EVP_MD_free(md);
    return ok;
}

static int sigDigestVerifyUpdate(void *c, const unsigned char *data, size_t length)
{
    struct sig *s = c;
    return s->md && EVP_DigestUpdate(s->md, data, length);
}

static int sigDigestVerifyFinal(void *c, const unsigned char *signature, size_t signatureLength)
{
    struct sig *s = c;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!s->md || !EVP_DigestFinal_ex(s->md, digest, &length))
        return 0;
    return sigVerify(c, signature, signatureLength, digest, length);
}

#define SIGNATURE_FUNCTIONS \
    { OSSL_FUNC_SIGNATURE_NEWCTX, (void (*)(void))sigNew }, \
    { OSSL_FUNC_SIGNATURE_FREECTX, (void (*)(void))sigFree }, \
    { OSSL_FUNC_SIGNATURE_DUPCTX, (void (*)(void))sigDup }, \
    { OSSL_FUNC_SIGNATURE_VERIFY_INIT, (void (*)(void))sigVerifyInit }, \
    { OSSL_FUNC_SIGNATURE_VERIFY, (void (*)(void))sigVerify }, \
    { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_INIT, (void (*)(void))sigDigestVerifyInit }, \
    { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_UPDATE, (void (*)(void))sigDigestVerifyUpdate }, \
    { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_FINAL, (void (*)(void))sigDigestVerifyFinal }, \
    { OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS, (void (*)(void))sigSetParams }, \
    { OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS, (void (*)(void))sigSettableParams }, \
    { 0, NULL }

static const OSSL_DISPATCH signature[] = { SIGNATURE_FUNCTIONS };

/* ---- the provider ------------------------------------------------------------------ */

static const OSSL_ALGORITHM keymgmts[] = {
    { "RSA:rsaEncryption:1.2.840.113549.1.1.1", PROPS, rsaKeymgmt, "RSA public keys, checked by opentls.key/1" },
    { "EC:id-ecPublicKey:1.2.840.10045.2.1", PROPS, ecKeymgmt, "EC keys for opentls.key/1" },
    { "X25519:1.3.101.110", PROPS, x25519Keymgmt, "X25519 keys for opentls.key/1" },
    { NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM keyexchs[] = {
    { "ECDH", PROPS, keyexch, "ECDH by opentls.key/1" },
    { "X25519", PROPS, keyexch, "X25519 by opentls.key/1" },
    { NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM signatures[] = {
    { "RSA:rsaEncryption:1.2.840.113549.1.1.1", PROPS, signature, "RSA verify by opentls.key/1" },
    { "ECDSA", PROPS, signature, "ECDSA verify by opentls.key/1" },
    { NULL, NULL, NULL, NULL }
};

static const OSSL_ALGORITHM *query(void *provctx, int operation, int *noCache)
{
    (void)provctx;
    *noCache = 0;
    switch (operation) {
    case OSSL_OP_KEYMGMT: return keymgmts;
    case OSSL_OP_KEYEXCH: return keyexchs;
    case OSSL_OP_SIGNATURE: return signatures;
    }
    return NULL;
}

static void teardown(void *provctx) { (void)provctx; }

static const OSSL_DISPATCH providerFunctions[] = {
    { OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void))query },
    { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void))teardown },
    { 0, NULL }
};

static int providerInit(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in, const OSSL_DISPATCH **out, void **provctx)
{
    (void)handle;
    (void)in;                         /* see the top: never called */
    *out = providerFunctions;
    *provctx = (void *)providerFunctions;
    return 1;
}

int opentls_install(opentls_transport t)
{
    static int installed;
    transport = t;
    otk_propq = DEFAULT_PROPS;        /* our own fall-back work stays with the default provider */
    if (installed)
        return 1;
    if (!OSSL_PROVIDER_add_builtin(NULL, "obkey", providerInit)
        || !OSSL_PROVIDER_load(NULL, "default") || !OSSL_PROVIDER_load(NULL, "obkey")
        || !EVP_set_default_properties(NULL, "?" PROPS))
        return 0;
    installed = 1;
    return 1;
}
