/*
 * tlstest HOST...: a TLS handshake with each host through the obkey
 * provider, the service played by host/opentls_key.c in this process.
 * Checks the certificate chain and host name as a browser does.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include "opentls.h"

/* TLSTEST=down: the service never answers (the work is done here);
 * TLSTEST=bad: it says every signature is bad (every handshake must fail). */
static int service(uint16_t op, uint32_t arg, const uint32_t extra[4], struct otk_buffer buf[4], uint32_t *result, uint32_t *aux)
{
    const char *mode = getenv("TLSTEST");
    int status;
    if (mode && !strcmp(mode, "down"))
        return -6;
    status = otk_call(op, arg, extra, buf, result, aux);
    if (mode && !strcmp(mode, "bad") && op == OTK_VERIFY)
        *result = 0;
    return status;
}

int main(int argc, char **argv)
{
    int i, failed = 0;
    SSL_CTX *ctx;
    if (!opentls_install(service)) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (argc > 2 && !strncmp(argv[1], "groups=", 7)) {
        SSL_CTX_set1_groups_list(ctx, argv[1] + 7);
        argv++;
        argc--;
    }
    for (i = 1; i < argc; i++) {
        char target[300];
        BIO *b;
        SSL *ssl;
        struct opentls_stats before = opentls_stats;
        snprintf(target, sizeof target, "%s:443", argv[i]);
        b = BIO_new_ssl_connect(ctx);
        BIO_get_ssl(b, &ssl);
        SSL_set_tlsext_host_name(ssl, argv[i]);
        SSL_set1_host(ssl, argv[i]);
        BIO_set_conn_hostname(b, target);
        if (BIO_do_handshake(b) <= 0) {
            printf("%-24s FAILED\n", argv[i]);
            ERR_print_errors_fp(stdout);
            failed++;
        } else {
            printf("%-24s %s %s, group %s, verify %ld: %lu verifies, %lu keygens, %lu derives (%lu sent, %lu here)\n",
                   argv[i], SSL_get_version(ssl), SSL_get_cipher(ssl), SSL_group_to_name(ssl, SSL_get_negotiated_group(ssl)),
                   SSL_get_verify_result(ssl),
                   opentls_stats.verifies - before.verifies, opentls_stats.keygens - before.keygens,
                   opentls_stats.derives - before.derives, opentls_stats.sent - before.sent, opentls_stats.local - before.local);
        }
        BIO_free_all(b);
    }
    SSL_CTX_free(ctx);
    return failed;
}
