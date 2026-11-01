/*
 * openservice: the Cradles in the nursery (mdns.c).
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#ifndef OPENSERVICE_MDNS_H
#define OPENSERVICE_MDNS_H

#include <exec/types.h>

struct mdns_cradle {
    char instance[128];   /* "daletop._amicradle._tcp.local" */
    char target[128];     /* the SRV target, "daletop.local" */
    UWORD port;
    ULONG address;        /* IPv4, host order; 0 when no A record came */
    char services[256];   /* svc=: "opentls.key/1,media.decode/1" */
    char fingerprint[72]; /* fp= */
    char name[64];        /* name= */
};

/* Asks the LAN for seconds (at least 1), with SocketBase open. Returns the
 * number of Cradles put in list, or -1 with no socket. Stops early on any
 * of breakSignals. */
int mdns_browse(int seconds, struct mdns_cradle *list, int max, ULONG breakSignals);

/* 1 when the comma-separated list holds name. */
int mdns_offers(const struct mdns_cradle *c, const char *name);

#endif
