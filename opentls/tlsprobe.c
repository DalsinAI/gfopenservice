/* tlsprobe [SERVICE] URL...: how long each TLS handshake takes through curl
 * and AmiSSL, with the key maths on the 68k, or with SERVICE sent to
 * opentls.key/1 through gfopenservice.device (a services board or a paired
 * Cradle). Each URL gets a fresh handle, so a full handshake.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
#include "ob_network.h"
#include "opentls.h"
#include "opentls_amiga.h"

extern void ob_quiet_requesters(void);

static size_t sink(char *p, size_t s, size_t n, void *u) { (void)p; (void)u; return s * n; }

int main(int argc, char **argv)
{
    int i = 1, service = 0;
    if (argc > 1 && !strcmp(argv[1], "SERVICE")) {
        service = 1;
        i++;
    }
    ob_quiet_requesters();
    if (!ob_network_open()) {
        printf("no network\n");
        return 20;
    }
    if (service) {
        int where = opentls_amiga_open();
        printf("opentls.key/1: %s\n", where == 1 ? "on a board in this machine" : where == 2 ? "on a Cradle on the LAN" : "not offered, all on the 68k");
    }
    curl_global_init(CURL_GLOBAL_ALL);
    for (; i < argc; i++) {
        CURL *h = curl_easy_init();
        curl_off_t connect = 0, tls = 0, total = 0;
        struct opentls_stats before = opentls_stats;
        CURLcode rc;
        curl_easy_setopt(h, CURLOPT_URL, argv[i]);
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        rc = curl_easy_perform(h);
        curl_easy_getinfo(h, CURLINFO_CONNECT_TIME_T, &connect);
        curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME_T, &tls);
        curl_easy_getinfo(h, CURLINFO_TOTAL_TIME_T, &total);
        printf("%-34s %s: TLS %ld ms, total %ld ms; %lu verifies, %lu keygens, %lu derives (%lu sent, %lu on the 68k)\n",
               argv[i], rc ? curl_easy_strerror(rc) : "ok", (long)((tls - connect) / 1000), (long)(total / 1000),
               opentls_stats.verifies - before.verifies, opentls_stats.keygens - before.keygens,
               opentls_stats.derives - before.derives, opentls_stats.sent - before.sent, opentls_stats.local - before.local);
        curl_easy_cleanup(h);
    }
    curl_global_cleanup();
    if (service)
        opentls_amiga_close();
    ob_network_close();
    return 0;
}
