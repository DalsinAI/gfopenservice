/*
 * Nursery: lists the services this Amiga can hand work to.
 *
 * First the boards with the services block (AmigaChrome Team $DA15, any product): the
 * services card, CPU cores, FPU or TPU boards, virtual ones on AmiCradle on
 * a PC or on our appliance, or real ones. Then any
 * Cradle on the LAN that offers services, found with one mDNS question for
 * _amicradle._tcp.local. A Cradle lists what it offers in its TXT record:
 *   svc=opentls.key/1,media.decode/1
 *   fp=<pairing fingerprint>
 *   name=<what to call it>
 *
 * Usage: Nursery [TIME=<seconds>] [ALL] [PAIR=<Cradle>]
 *   TIME  how long to listen for answers (default 2)
 *   ALL   also list Cradles that offer no services
 *   PAIR  pair with the Cradle of that name (or fingerprint): both show a
 *         six-digit code, and once they match the link is sealed with the
 *         key they agreed (docs/PAIRING.md)
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "openservice.h"
#include "mdns.h"
#include "oscrypto.h"
#include "osrandom.h"

const char version[] __attribute__((used)) = "$VER: Nursery 0.1 (5.10.2026)";

struct Library *SocketBase;
struct ExpansionBase *ExpansionBase;

#define MAX_FOUND 16

static struct mdns_cradle found[MAX_FOUND];
static int foundCount;

/* ---- the boards ---- */

static const char *className(ULONG c)
{
    switch (c) {
    case OPENSERVICE_CLASS_SERVICES: return "services card";
    case OPENSERVICE_CLASS_CORES: return "CPU cores";
    case OPENSERVICE_CLASS_FPU: return "FPU";
    case OPENSERVICE_CLASS_TPU: return "TPU";
    }
    return "board";
}

static void listBoards(void)
{
    struct ConfigDev *cd = NULL;
    int any = 0;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 37);
    if (!ExpansionBase)
        return;
    while ((cd = FindConfigDev(cd, OPENSERVICE_MANUFACTURER, -1))) {
        const volatile ULONG *regs = (const volatile ULONG *)cd->cd_BoardAddr;
        if ((cd->cd_Rom.er_Type & ERTF_MEMLIST) || (cd->cd_Flags & CDF_SHUTUP) || regs[0] != OPENSERVICE_MAGIC)
            continue;
        printf("Board:   %s (product %u) at $%08lx, version %lu%s\n", className(regs[8]), cd->cd_Rom.er_Product,
               (unsigned long)cd->cd_BoardAddr, (unsigned long)regs[1], regs[7] & 1 ? ", runs on the x86 or ARM64 cores" : "");
        any = 1;
    }
    if (!any)
        printf("Board:   none\n");
    CloseLibrary((struct Library *)ExpansionBase);
}

/* ---- mDNS ---- */

static void listCradles(int all)
{
    int i, shown = 0;
    for (i = 0; i < foundCount; i++) {
        struct mdns_cradle *c = &found[i];
        char label[128];
        char *dot;
        if (!c->services[0] && !all)
            continue;
        strncpy(label, c->name[0] ? c->name : c->instance, sizeof label - 1);
        label[sizeof label - 1] = 0;
        if (!c->name[0] && (dot = strstr(label, "._amicradle")))
            *dot = 0;
        printf("Cradle:  %s", label);
        if (c->address)
            printf(" at %lu.%lu.%lu.%lu:%u", (unsigned long)(c->address >> 24), (unsigned long)((c->address >> 16) & 255), (unsigned long)((c->address >> 8) & 255), (unsigned long)(c->address & 255), c->port);
        else if (c->target[0])
            printf(" at %s:%u", c->target, c->port);
        printf("\n         services: %s\n", c->services[0] ? c->services : "none");
        {
            UBYTE key[32];
            printf("         paired:   %s\n", openservice_psk(c->fingerprint, key) ? "yes, sealed"
                   : openservice_paired(c->fingerprint) ? "yes, unsealed (a test Cradle)" : "no (Nursery PAIR=...)");
            memset(key, 0, sizeof key);
        }
        shown++;
    }
    if (!shown)
        printf("LAN:     no Cradle offering services\n");
}

/* ---- pairing ---- */

static int sendAll(LONG s, const void *data, ULONG length)
{
    const UBYTE *p = data;
    while (length) {
        LONG n = send(s, (APTR)p, length, 0);
        if (n <= 0)
            return 0;
        p += n;
        length -= n;
    }
    return 1;
}

static int recvAll(LONG s, void *data, ULONG length)
{
    UBYTE *p = data;
    while (length) {
        LONG n = recv(s, p, length, 0);
        if (n <= 0)
            return 0;
        p += n;
        length -= n;
    }
    return 1;
}

static void toHex(const UBYTE *in, int length, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < length; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * length] = 0;
}

/* Replaces fingerprint's line in file with line (both ENV: and ENVARC:). */
static void keepLine(const char *file, const char *fingerprint, const char *line)
{
    static char old[4096];
    char text[200];
    LONG used = 0;
    BPTR f = Open((CONST_STRPTR)file, MODE_OLDFILE);
    old[0] = 0;
    if (f) {
        while (FGets(f, (STRPTR)text, sizeof text)) {
            size_t n = strcspn(text, " \t\r\n");
            if ((n == strlen(fingerprint) && !strncmp(text, fingerprint, n)) || used + strlen(text) >= sizeof old)
                continue;
            strcpy(old + used, text);
            used += strlen(text);
        }
        Close(f);
    }
    f = Open((CONST_STRPTR)file, MODE_NEWFILE);
    if (f) {
        FPuts(f, (CONST_STRPTR)old);
        FPuts(f, (CONST_STRPTR)line);
        Close(f);
    }
}

static void makeDirs(void)
{
    BPTR lock;
    if ((lock = CreateDir((CONST_STRPTR)"ENV:GFOpenService")))
        UnLock(lock);
    if ((lock = CreateDir((CONST_STRPTR)"ENVARC:GFOpenService")))
        UnLock(lock);
}

/* This Amiga's identity, made the first time. */
static int amigaId(UBYTE id[16])
{
    char hex[40];
    BPTR f;
    if (openservice_amiga_id(id))
        return 1;
    os_random(id, 16, 1);
    toHex(id, 16, hex);
    strcat(hex, "\n");
    makeDirs();
    if ((f = Open((CONST_STRPTR)OPENSERVICE_ID_FILE, MODE_NEWFILE))) {
        FPuts(f, (CONST_STRPTR)hex);
        Close(f);
    }
    if ((f = Open((CONST_STRPTR)OPENSERVICE_ID_KEEP, MODE_NEWFILE))) {
        FPuts(f, (CONST_STRPTR)hex);
        Close(f);
    }
    return openservice_amiga_id(id);
}

static int names(const struct mdns_cradle *c, const char *what)
{
    return (c->name[0] && !stricmp(c->name, what)) || !stricmp(c->fingerprint, what)
        || !strnicmp(c->instance, what, strlen(what));
}

static int pair(const char *what)
{
    struct mdns_cradle *c = NULL;
    struct sockaddr_in to;
    UBYTE hello[52], reply[52], secret[32], mine[32], ours[48], theirs[48], salt[32], psk[32], check[32], yes, sas[32];
    char line[200], hexKey[70], answer[16], cradleHex[40];
    struct os_sha256 h;
    ULONG code;
    LONG s;
    int i;
    for (i = 0; i < foundCount && !c; i++)
        if (names(&found[i], what))
            c = &found[i];
    if (!c || !c->address) {
        printf("No Cradle called %s on the LAN (Nursery ALL lists them).\n", what);
        return RETURN_WARN;
    }
    if (!amigaId(hello + 4)) {
        printf("Cannot keep this Amiga's identity in ENV:GFOpenService.\n");
        return RETURN_FAIL;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&to, 0, sizeof to);
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = c->port;
    to.sin_addr.s_addr = c->address;
    if (s < 0 || connect(s, (struct sockaddr *)&to, sizeof to) < 0) {
        printf("Cannot reach %s.\n", c->name[0] ? c->name : c->instance);
        if (s >= 0)
            CloseSocket(s);
        return RETURN_WARN;
    }
    memcpy(hello, "OSP1", 4);
    memset(hello + 20, 0, 32);
    if (GetVar((CONST_STRPTR)"HOSTNAME", (STRPTR)hello + 20, 32, 0) <= 0)
        strcpy((char *)hello + 20, "Amiga");
    printf("Pairing with %s: making a key (this takes a while on a 68k)...\n", c->name[0] ? c->name : c->instance);
    if (!sendAll(s, hello, 52) || !recvAll(s, reply, 52) || memcmp(reply, "OSP1", 4)) {
        printf("The Cradle did not take the request.\n");
        CloseSocket(s);
        return RETURN_WARN;
    }
    os_random(secret, 32, 1);
    os_x25519_base(ours, secret);
    os_random(ours + 32, 16, 0);
    if (!sendAll(s, ours, 48) || !recvAll(s, theirs, 48)) {
        CloseSocket(s);
        return RETURN_WARN;
    }
    os_sha256(theirs, 48, check);
    if (memcmp(check, reply + 20, 32)) {
        printf("The Cradle's key does not match what it promised: not pairing.\n");
        CloseSocket(s);
        return RETURN_WARN;
    }
    os_x25519(mine, secret, theirs);
    memset(secret, 0, sizeof secret);
    os_sha256_init(&h);
    os_sha256_update(&h, "openservice sas", 15);
    os_sha256_update(&h, ours, 32);
    os_sha256_update(&h, theirs, 32);
    os_sha256_update(&h, ours + 32, 16);
    os_sha256_update(&h, theirs + 32, 16);
    os_sha256_final(&h, sas);
    code = ((ULONG)sas[0] << 24 | (ULONG)sas[1] << 16 | (ULONG)sas[2] << 8 | sas[3]) % 1000000UL;
    printf("The Cradle should show the code %03lu %03lu. Does it? (y/n) ", (unsigned long)(code / 1000), (unsigned long)(code % 1000));
    fflush(stdout);
    yes = fgets(answer, sizeof answer, stdin) && (answer[0] == 'y' || answer[0] == 'Y');
    if (!sendAll(s, &yes, 1) || !recvAll(s, answer, 1) || !yes || answer[0] != 1) {
        printf(yes ? "The Cradle did not accept it: not paired.\n" : "Not paired.\n");
        CloseSocket(s);
        return RETURN_WARN;
    }
    CloseSocket(s);
    memcpy(salt, ours + 32, 16);
    memcpy(salt + 16, theirs + 32, 16);
    os_hkdf_sha256(mine, 32, salt, 32, "openservice pair v1", psk, 32);
    memset(mine, 0, sizeof mine);
    toHex(reply + 4, 16, cradleHex);
    toHex(psk, 32, hexKey);
    memset(psk, 0, sizeof psk);
    sprintf(line, "%s %s %s\n", cradleHex, hexKey, c->name[0] ? c->name : "Cradle");
    makeDirs();
    keepLine(OPENSERVICE_PAIRED_FILE, cradleHex, line);
    keepLine(OPENSERVICE_PAIRED_KEEP, cradleHex, line);
    memset(line, 0, sizeof line);
    memset(hexKey, 0, sizeof hexKey);
    printf("Paired with %s. Its services are sealed from now on.\n", c->name[0] ? c->name : c->instance);
    return RETURN_OK;
}

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rd = ReadArgs((CONST_STRPTR)"TIME/N,ALL/S,PAIR/K", args, NULL);
    int seconds = 2, rc = RETURN_OK;
    if (!rd) {
        PrintFault(IoErr(), (CONST_STRPTR)"Nursery");
        return RETURN_FAIL;
    }
    if (args[0])
        seconds = (int)*(LONG *)args[0];
    if (seconds < 1)
        seconds = 1;
    listBoards();
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase)
        printf("LAN:     no network\n");
    else {
        foundCount = mdns_browse(seconds, found, MAX_FOUND, SIGBREAKF_CTRL_C);
        if (foundCount < 0) {
            printf("LAN:     no socket\n");
            foundCount = 0;
        }
        if (args[2])
            rc = pair((const char *)args[2]);
        else
            listCradles(args[1] != 0);
        CloseLibrary(SocketBase);
    }
    os_random_close();
    FreeArgs(rd);
    return rc;
}
