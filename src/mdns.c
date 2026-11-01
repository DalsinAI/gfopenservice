/*
 * openservice: find Cradles in the nursery with one mDNS question for
 * _amigachrome._tcp.local (see mdns.h).
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "mdns.h"

#define MDNS_PORT 5353
#define MDNS_GROUP 0xe00000fbUL   /* 224.0.0.251 */

static struct mdns_cradle *found;

/* ASCII only, as DNS names compare (no locale, no C library start-up). */
static int sameName(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}
static int foundCount, foundMax;

static const char serviceType[] = "_amigachrome._tcp.local";

static int putName(UBYTE *p, const char *name)
{
    int n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        int len = dot ? (int)(dot - name) : (int)strlen(name);
        p[n++] = (UBYTE)len;
        memcpy(p + n, name, len);
        n += len;
        name += len;
        if (*name == '.')
            name++;
    }
    p[n++] = 0;
    return n;
}

/* Reads a possibly compressed name at *at into out; moves *at past it. */
static int getName(const UBYTE *msg, int length, int *at, char *out, int outSize)
{
    int p = *at, jumped = 0, hops = 0, o = 0;
    for (;;) {
        int len;
        if (p >= length || ++hops > 64)
            return 0;
        len = msg[p];
        if ((len & 0xc0) == 0xc0) {
            if (p + 1 >= length)
                return 0;
            if (!jumped)
                *at = p + 2;
            jumped = 1;
            p = (len & 0x3f) << 8 | msg[p + 1];
            continue;
        }
        p++;
        if (!len)
            break;
        if (p + len > length)
            return 0;
        if (o && o < outSize - 1)
            out[o++] = '.';
        for (; len > 0; len--, p++)
            if (o < outSize - 1)
                out[o++] = (char)msg[p];
    }
    out[o] = 0;
    if (!jumped)
        *at = p;
    return 1;
}

static struct mdns_cradle *cradleFor(const char *instance, int add)
{
    int i;
    for (i = 0; i < foundCount; i++)
        if (sameName(found[i].instance, instance))
            return &found[i];
    if (!add || foundCount == foundMax)
        return NULL;
    memset(&found[foundCount], 0, sizeof found[0]);
    strncpy(found[foundCount].instance, instance, sizeof found[0].instance - 1);
    return &found[foundCount++];
}

static void takeTxt(struct mdns_cradle *c, const UBYTE *p, int length)
{
    while (length > 0) {
        int len = *p++;
        char item[256];
        length--;
        if (len > length)
            return;
        memcpy(item, p, len);
        item[len] = 0;
        if (!strncmp(item, "svc=", 4))
            strncpy(c->services, item + 4, sizeof c->services - 1);
        else if (!strncmp(item, "fp=", 3))
            strncpy(c->fingerprint, item + 3, sizeof c->fingerprint - 1);
        else if (!strncmp(item, "name=", 5))
            strncpy(c->name, item + 5, sizeof c->name - 1);
        p += len;
        length -= len;
    }
}

/* Every A record for c's target is kept, not the last to come: any host on
 * the LAN can answer for the name, so the device tries them all and uses the
 * one that proves the pairing key. */
static void addAddress(struct mdns_cradle *c, ULONG a)
{
    int i;
    if (!a)
        return;
    for (i = 0; i < c->addressCount; i++)
        if (c->addresses[i] == a)
            return;
    if (c->addressCount == MDNS_ADDRESSES)
        return;
    c->addresses[c->addressCount++] = a;
    c->address = c->addresses[0];
}

static void takeAnswer(const UBYTE *msg, int length)
{
    int at = 12, i, records;
    int questions = msg[4] << 8 | msg[5];
    if (length < 12 || !(msg[2] & 0x80))
        return;                                        /* not a response */
    records = (msg[6] << 8 | msg[7]) + (msg[8] << 8 | msg[9]) + (msg[10] << 8 | msg[11]);
    for (i = 0; i < questions; i++) {
        char skip[256];
        if (!getName(msg, length, &at, skip, sizeof skip))
            return;
        at += 4;
    }
    /* Twice: the first pass finds our instances (PTR), the second fills them
     * in, whatever order the records came in. */
    {
        int pass, start = at;
        for (pass = 0; pass < 2; pass++) {
            at = start;
            for (i = 0; i < records; i++) {
                char name[256];
                int type, rdlength, rdata;
                struct mdns_cradle *c;
                if (!getName(msg, length, &at, name, sizeof name) || at + 10 > length)
                    return;
                type = msg[at] << 8 | msg[at + 1];
                rdlength = msg[at + 8] << 8 | msg[at + 9];
                rdata = at + 10;
                at = rdata + rdlength;
                if (at > length)
                    return;
                if (pass == 0) {
                    if (type == 12 && sameName(name, serviceType)) {
                        char instance[256];
                        int p = rdata;
                        if (getName(msg, length, &p, instance, sizeof instance))
                            cradleFor(instance, 1);
                    }
                    continue;
                }
                if (type == 33 && (c = cradleFor(name, 0)) && rdlength >= 7) {
                    int p = rdata + 6;
                    c->port = (UWORD)(msg[rdata + 4] << 8 | msg[rdata + 5]);
                    getName(msg, length, &p, c->target, sizeof c->target);
                } else if (type == 16 && (c = cradleFor(name, 0))) {
                    takeTxt(c, msg + rdata, rdlength);
                } else if (type == 1 && rdlength == 4) {
                    ULONG a = (ULONG)msg[rdata] << 24 | (ULONG)msg[rdata + 1] << 16 | (ULONG)msg[rdata + 2] << 8 | msg[rdata + 3];
                    int k;
                    for (k = 0; k < foundCount; k++)
                        if (sameName(found[k].target, name))
                            addAddress(&found[k], a);
                }
            }
        }
    }
}

static int sendQuestion(LONG s, int legacy)
{
    UBYTE q[64];
    int n = 12;
    struct sockaddr_in to;
    memset(q, 0, sizeof q);
    if (legacy)
        q[1] = 0x42;                                   /* any id: answers come back to our port */
    q[5] = 1;                                          /* one question */
    n += putName(q + n, serviceType);
    q[n++] = 0; q[n++] = 12;                           /* PTR */
    q[n++] = 0; q[n++] = 1;                            /* IN */
    memset(&to, 0, sizeof to);
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = MDNS_PORT;
    to.sin_addr.s_addr = MDNS_GROUP;
    return sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof to) == n;
}

/* Ticks (1/50 s) since t0, by dos.library's clock. */
static LONG ticksSince(const struct DateStamp *t0)
{
    struct DateStamp t;
    DateStamp(&t);
    return ((t.ds_Days - t0->ds_Days) * 1440 + (t.ds_Minute - t0->ds_Minute)) * 60 * TICKS_PER_SECOND
           + (t.ds_Tick - t0->ds_Tick);
}

int mdns_browse(int seconds, struct mdns_cradle *list, int max, ULONG breakSignals)
{
    struct sockaddr_in me;
    struct ip_mreq group;
    LONG s, one = 1;
    int legacy = 0, round;
    found = list;
    foundMax = max;
    foundCount = 0;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    memset(&me, 0, sizeof me);
    me.sin_len = sizeof me;
    me.sin_family = AF_INET;
    me.sin_port = MDNS_PORT;
    if (bind(s, (struct sockaddr *)&me, sizeof me) < 0) {
        /* Someone else has 5353: ask from our own port instead, and
         * responders answer us directly (RFC 6762, 6.7). */
        legacy = 1;
        me.sin_port = 0;
        bind(s, (struct sockaddr *)&me, sizeof me);
    } else {
        group.imr_multiaddr.s_addr = MDNS_GROUP;
        group.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group) < 0)
            ;
    }
    for (round = 0; round < seconds * 2; round++) {
        struct timeval wait;
        struct DateStamp start;
        fd_set ready;
        ULONG signals = 0;
        if (round % 2 == 0)
            sendQuestion(s, legacy);
        /* Half a second a round, however much mDNS the LAN carries: every
         * responder's chatter reaches this socket, so a wait that started
         * again at each packet could go on for ever. */
        DateStamp(&start);
        for (;;) {
            UBYTE msg[1500];
            LONG n, left = TICKS_PER_SECOND / 2 - ticksSince(&start);
            if (left <= 0)
                break;
            FD_ZERO(&ready);
            FD_SET(s, &ready);
            wait.tv_sec = 0;
            wait.tv_usec = left * (1000000 / TICKS_PER_SECOND);
            signals = breakSignals;
            if (WaitSelect(s + 1, &ready, NULL, NULL, &wait, &signals) <= 0 || (signals & breakSignals))
                break;
            n = recvfrom(s, msg, sizeof msg, 0, NULL, NULL);
            if (n > 0)
                takeAnswer(msg, (int)n);
        }
        if (signals & breakSignals)
            break;
    }
    CloseSocket(s);
    return foundCount;
}

int mdns_offers(const struct mdns_cradle *c, const char *name)
{
    const char *p = c->services;
    size_t n = strlen(name);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !strncmp(p, name, n))
            return 1;
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}
