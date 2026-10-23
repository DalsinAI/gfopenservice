/*
 * wfnet on the Amiga (wfnet_amiga.h): the machine side of
 * include/openweb/wfnet.h with bsdsocket.library (any stack: OpenSocket,
 * Roadshow, AmiTCP) and opentls.library 1.x for TLS, ALPN included.
 * Every library base lives in a struct wfna, none in globals, so any
 * number of tasks can each have their own.
 *
 * Compressed bodies are undone by the datatypes (src/wfnet_dec.c):
 * datatypes.library is opened the first time a coded body or WFN_F_DECODE
 * comes, and stream.datatype's gzip.datatype and brotli.datatype do the
 * work. Without them the body comes as the server sent it.
 *
 * Names are resolved with gethostbyname(), which blocks the task for as
 * long as the stack takes; everything after that is non-blocking.
 * WaitSelect() also waits on Ctrl-C and on the signals the caller names
 * (wfna_wait), so a program's own event loop can sit on it.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#define __NOLIBBASE__
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/filio.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#include <libraries/opentls.h>
#include <proto/opentls.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <proto/datatypes.h>
#include <datatypes/streamclass.h>

#include "openweb/wfnet.h"
#include "wfnet_amiga.h"
#include "wfnet_dec.h"

/* The programs' own SysBase and DOSBase; the network's bases are ours. */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
#define SocketBase (e->socketbase)
#define OpenTLSBase (e->tlsbase)
#define DataTypesBase (e->dtbase)

/* bsdsocket.library's error numbers (BSD's), whatever the C library calls its own. */
#define B_EINTR       4
#define B_EWOULDBLOCK 35
#define B_EINPROGRESS 36

#define PENDING 8               /* connections under way that have other addresses to try */
#define ADDRS   4               /* and how many of those each keeps */

struct wfna {
    struct Library *socketbase;
    struct Library *tlsbase;
    struct OTContext *ctx;
    struct Library *dtbase;     /* datatypes.library, once a body is to be decoded */
    UBYTE dt_tried;             /* it was looked for */
    UBYTE dt_seen;              /* which datatypes are there (wfn_dec_accept) */
    ULONG sigs;                 /* the caller's signals to wait on as well */
    ULONG got;                  /* which of them came */
    ULONG seed;
    struct wfn *w;
    struct {
        LONG sock;
        UWORD port, left;       /* left: addresses still to try, the last first; 0, a free entry */
        ULONG addr[ADDRS];
    } pend[PENDING];
};

struct tls {
    struct OTConnection *conn;
    LONG status;                /* the handshake's answer */
};

static void *a_alloc(void *env, unsigned long n)
{
    (void)env;
    return AllocVec(n, MEMF_ANY | MEMF_CLEAR);
}

static void a_release(void *env, void *p)
{
    (void)env;
    FreeVec(p);
}

static unsigned long a_now(void *env)
{
    struct DateStamp d;
    (void)env;
    DateStamp(&d);
    return ((unsigned long)d.ds_Days * 1440UL + (unsigned long)d.ds_Minute) * 60000UL + (unsigned long)d.ds_Tick * 20UL;
}

/* A non-blocking connection to addr (network order) under way: the socket, or -1. */
static LONG a_start(struct wfna *e, ULONG addr, unsigned port)
{
    struct sockaddr_in sa;
    LONG s, on = 1;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons((UWORD)port);
    sa.sin_addr.s_addr = addr;
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    IoctlSocket(s, FIONBIO, (APTR)&on);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0 && Errno() != B_EINPROGRESS) {
        CloseSocket(s);
        return -1;
    }
    return s;
}

static void a_forget(struct wfna *e, long sock)
{
    int i;
    for (i = 0; i < PENDING; i++)
        if (e->pend[i].left && e->pend[i].sock == sock)
            e->pend[i].left = 0;
}

static long a_connect(void *env, const char *host, unsigned port, int *error)
{
    struct wfna *e = env;
    struct hostent *he;
    ULONG addr;
    LONG s = -1;
    int i = 0, j, k, n;
    if (!(he = gethostbyname(host)) || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
        *error = WFN_E_DNS;
        return -1;
    }
    while (s < 0 && he->h_addr_list[i]) {
        memcpy(&addr, he->h_addr_list[i++], 4);
        s = a_start(e, addr, port);
    }
    if (s < 0) {
        *error = WFN_E_CONNECT;
        return -1;
    }
    /* the rest, for when this one fails on the way (a_connected) */
    for (n = 0; n < ADDRS && he->h_addr_list[i + n]; n++)
        ;
    for (k = 0; n && k < PENDING && e->pend[k].left; k++)
        ;
    if (n && k < PENDING) {
        e->pend[k].sock = s, e->pend[k].port = (UWORD)port, e->pend[k].left = (UWORD)n;
        for (j = 0; j < n; j++)
            memcpy(&e->pend[k].addr[n - 1 - j], he->h_addr_list[i + j], 4);
    }
    return s;
}

static int a_connected(void *env, long sock)
{
    struct wfna *e = env;
    struct timeval tv;
    fd_set wf;
    LONG err = 0, s;
    socklen_t len = sizeof err;
    int i;
    tv.tv_sec = tv.tv_usec = 0;
    FD_ZERO(&wf);
    FD_SET(sock, &wf);
    if (WaitSelect(sock + 1, NULL, &wf, NULL, &tv, NULL) <= 0)
        return WFN_AGAIN_W;
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        /* the next address, on the same socket number */
        for (i = 0; i < PENDING; i++)
            while (e->pend[i].left && e->pend[i].sock == sock) {
                if ((s = a_start(e, e->pend[i].addr[--e->pend[i].left], e->pend[i].port)) < 0)
                    continue;
                if (Dup2Socket(s, sock) >= 0) {
                    CloseSocket(s);
                    return WFN_AGAIN_W;
                }
                CloseSocket(s);
            }
        return WFN_E_CONNECT;
    }
    a_forget(e, sock);
    return 0;
}

static long a_send(void *env, long sock, const void *p, unsigned long n)
{
    struct wfna *e = env;
    LONG r = send(sock, (APTR)p, (LONG)n, 0);
    if (r >= 0)
        return r;
    return Errno() == B_EWOULDBLOCK || Errno() == B_EINTR ? WFN_AGAIN_W : WFN_FAIL;
}

static long a_recv(void *env, long sock, void *p, unsigned long n)
{
    struct wfna *e = env;
    LONG r = recv(sock, p, (LONG)n, 0);
    if (r >= 0)
        return r;
    return Errno() == B_EWOULDBLOCK || Errno() == B_EINTR ? WFN_AGAIN : WFN_FAIL;
}

static void a_close(void *env, long sock)
{
    struct wfna *e = env;
    a_forget(e, sock);
    CloseSocket(sock);
}

static int a_wait(void *env, const long *socks, const unsigned char *want, unsigned char *ready, int n, unsigned long ms)
{
    struct wfna *e = env;
    struct timeval tv;
    fd_set rf, wf;
    ULONG sigs = SIGBREAKF_CTRL_C | e->sigs;
    LONG top = -1, i;
    FD_ZERO(&rf);
    FD_ZERO(&wf);
    for (i = 0; i < n; i++) {
        if (want[i] & WFN_WANT_READ)
            FD_SET(socks[i], &rf);
        if (want[i] & WFN_WANT_WRITE)
            FD_SET(socks[i], &wf);
        if (socks[i] > top)
            top = socks[i];
        ready[i] = 0;
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    WaitSelect(top + 1, &rf, &wf, NULL, &tv, &sigs);
    for (i = 0; i < n; i++)
        ready[i] = (unsigned char)((FD_ISSET(socks[i], &rf) ? WFN_WANT_READ : 0) | (FD_ISSET(socks[i], &wf) ? WFN_WANT_WRITE : 0));
    e->got |= sigs & e->sigs;
    if (sigs & SIGBREAKF_CTRL_C) {
        SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);  /* left for the program to see */
        return WFN_BROKEN;
    }
    return sigs & e->sigs ? WFN_BROKEN : 0;
}

static int tls_lib(struct wfna *e)
{
    struct OTContextConfig config;
    LONG err = 0;
    if (!e->tlsbase && !(e->tlsbase = OpenLibrary((CONST_STRPTR)OPENTLSLIB_NAME, OPENTLSLIB_VERSION)))
        return 0;
    if (!e->ctx) {
        memset(&config, 0, sizeof config);
        config.Size = sizeof config;
        config.MinVersion = OT_TLS12;
        e->ctx = OT_NewContext(&config, &err);
    }
    return e->ctx != NULL;
}

static void *a_tls_new(void *env, long sock, const char *host, const char *alpn)
{
    struct wfna *e = env;
    struct tls *t;
    LONG err = 0;
    if (!tls_lib(e) || !(t = AllocVec(sizeof *t, MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    if (!(t->conn = OT_NewConnection(e->ctx, (CONST_STRPTR)host, &err))) {
        FreeVec(t);
        return NULL;
    }
    OT_SetSocket(t->conn, sock, e->socketbase);
    OT_SetALPN(t->conn, (CONST_STRPTR)alpn);
    t->status = OTERR_WOULDBLOCK;
    return t;
}

static int a_tls_handshake(void *env, void *tls)
{
    struct wfna *e = env;
    struct tls *t = tls;
    t->status = OT_Handshake(t->conn);
    if (t->status == OTERR_OK)
        return 0;
    return t->status == OTERR_WOULDBLOCK ? WFN_AGAIN : WFN_FAIL;
}

static long a_tls_read(void *env, void *tls, void *p, unsigned long n)
{
    struct wfna *e = env;
    LONG r = OT_Read(((struct tls *)tls)->conn, p, (LONG)n);
    if (r >= 0)
        return r;
    if (r == OTERR_CLOSED)
        return 0;                               /* closed without close_notify: the end, as on a socket */
    return r == OTERR_WOULDBLOCK ? WFN_AGAIN : WFN_FAIL;
}

static long a_tls_write(void *env, void *tls, const void *p, unsigned long n)
{
    struct wfna *e = env;
    LONG r = OT_Write(((struct tls *)tls)->conn, (CONST_APTR)p, (LONG)n);
    if (r > 0)
        return r;
    return r == OTERR_WOULDBLOCK || r == 0 ? WFN_AGAIN_W : WFN_FAIL;
}

static long a_tls_pending(void *env, void *tls)
{
    struct wfna *e = env;
    LONG r = OT_Pending(((struct tls *)tls)->conn);
    return r > 0 ? r : 0;
}

static unsigned long put(char *out, unsigned long at, unsigned long room, const char *s)
{
    unsigned long n = s ? strlen(s) : 0;
    if (at + n + 1 >= room)
        return at;
    memcpy(out + at, s, n);
    return at + n;
}

static unsigned long a_tls_info(void *env, void *tls, char *out, unsigned long room, int *verified)
{
    struct wfna *e = env;
    struct tls *t = tls;
    unsigned long at = 0;
    LONG v;
    *verified = 0;
    if (t->status == OTERR_OK) {
        v = OT_GetProtocol(t->conn);
        at = put(out, at, room, v == OT_TLS13 ? "tls=TLSv1.3\ncipher=" : "tls=TLSv1.2\ncipher=");
        at = put(out, at, room, (const char *)OT_GetCipher(t->conn));
        at = put(out, at, room, "\nverified=yes\n");
        *verified = 1;                          /* OTV_FULL: chain, dates and name all checked */
    } else {
        at = put(out, at, room, "error=");
        at = put(out, at, room, (const char *)OT_GetErrorText(t->conn));
        at = put(out, at, room, "\n");
    }
    if (OT_GetPeerName(t->conn)) {
        at = put(out, at, room, "subject=CN=");
        at = put(out, at, room, (const char *)OT_GetPeerName(t->conn));
        at = put(out, at, room, "\n");
    }
    out[at] = 0;
    return at;
}

static const char *a_tls_alpn(void *env, void *tls)
{
    struct wfna *e = env;
    CONST_STRPTR p = OT_GetALPN(((struct tls *)tls)->conn);
    return p ? (const char *)p : "";
}

static void a_tls_free(void *env, void *tls)
{
    struct wfna *e = env;
    struct tls *t = tls;
    if (t->status == OTERR_OK)
        OT_Close(t->conn);
    OT_FreeConnection(t->conn);
    FreeVec(t);
}

static void a_random(void *env, void *p, unsigned long n)
{
    struct wfna *e = env;
    unsigned char *b = p;
    if (tls_lib(e) && OT_Random(p, (LONG)n) == OTERR_OK)
        return;
    while (n--) {                               /* no OpenTLS: good enough for masks and nonces, not keys */
        e->seed = e->seed * 1103515245UL + 12345UL + a_now(env);
        *b++ = (unsigned char)(e->seed >> 16);
    }
}

static long a_getenv(void *env, const char *name, char *out, unsigned long room)
{
    LONG n;
    (void)env;
    n = GetVar((CONST_STRPTR)name, (STRPTR)out, (LONG)room, 0);
    return n >= 0 ? n : -1;
}

/* ---- the datatypes, for src/wfnet_dec.c -------------------------------- */

/* wfnet_dec.h's numbers are streamclass.h's. */
typedef char wfn_dt_numbers[SDTF_END == WFN_DT_END && SDTF_HUNGRY == WFN_DT_HUNGRY && SDTF_ERROR == WFN_DT_ERROR
                            && SDTW_AUTO == WFN_DTW_AUTO && SDTW_GZIP == WFN_DTW_GZIP ? 1 : -1];

void *wfn_dt_alloc(void *env, unsigned long n)
{
    return a_alloc(env, n);
}

void wfn_dt_release(void *env, void *p)
{
    a_release(env, p);
}

void *wfn_dt_new(void *env, const char *name, unsigned long wrapper)
{
    struct wfna *e = env;
    struct TagItem t[5];
    if (!e->dtbase) {
        if (e->dt_tried)
            return NULL;
        e->dt_tried = 1;
        if (!(e->dtbase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39)))
            return NULL;
    }
    t[0].ti_Tag = DTA_SourceType, t[0].ti_Data = DTST_RAM;
    t[1].ti_Tag = DTA_BaseName, t[1].ti_Data = (ULONG)name;
    t[2].ti_Tag = DTA_GroupID, t[2].ti_Data = GID_DATA;
    t[3].ti_Tag = SDTA_Wrapper, t[3].ti_Data = wrapper;
    t[4].ti_Tag = TAG_DONE;
    return NewDTObjectA(NULL, t);
}

int wfn_dt_feed(void *env, void *o, const unsigned char *p, unsigned long n)
{
    struct wfna *e = env;
    struct sdtFeed m;
    m.MethodID = SDTM_FEED;
    m.sdtf_GInfo = NULL;
    m.sdtf_Data = p;
    m.sdtf_Length = n;
    return DoDTMethodA(o, NULL, NULL, (Msg)&m) != 0;
}

unsigned long wfn_dt_take(void *env, void *o, unsigned char *p, unsigned long n, unsigned long *flags)
{
    struct wfna *e = env;
    struct sdtTake m;
    ULONG got;
    m.MethodID = SDTM_TAKE;
    m.sdtt_GInfo = NULL;
    m.sdtt_Buffer = p;
    m.sdtt_Length = n;
    m.sdtt_Actual = m.sdtt_Flags = 0;
    got = DoDTMethodA(o, NULL, NULL, (Msg)&m);
    *flags = m.sdtt_Flags;                      /* SDTF_ are WFN_DT_ */
    return got;
}

void wfn_dt_end(void *env, void *o)
{
    struct wfna *e = env;
    struct sdtEnd m;
    m.MethodID = SDTM_END;
    m.sdte_GInfo = NULL;
    DoDTMethodA(o, NULL, NULL, (Msg)&m);
}

void wfn_dt_dispose(void *env, void *o)
{
    struct wfna *e = env;
    DisposeDTObject(o);
}

static const char *a_dec_accept(void *env)
{
    return wfn_dec_accept(env, &((struct wfna *)env)->dt_seen);
}

struct wfna *wfna_new(unsigned long window)
{
    struct wfna *e = AllocVec(sizeof *e, MEMF_ANY | MEMF_CLEAR);
    struct wfn_io io;
    if (!e)
        return NULL;
    if (!(e->socketbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        FreeVec(e);
        return NULL;
    }
    memset(&io, 0, sizeof io);
    io.env = e;
    io.alloc = a_alloc;
    io.release = a_release;
    io.now = a_now;
    io.connect = a_connect;
    io.connected = a_connected;
    io.send = a_send;
    io.recv = a_recv;
    io.close = a_close;
    io.wait = a_wait;
    io.tls_new = a_tls_new;
    io.tls_handshake = a_tls_handshake;
    io.tls_read = a_tls_read;
    io.tls_write = a_tls_write;
    io.tls_pending = a_tls_pending;
    io.tls_info = a_tls_info;
    io.tls_alpn = a_tls_alpn;
    io.tls_free = a_tls_free;
    io.random = a_random;
    io.getenv = a_getenv;
    io.dec_new = wfn_dec_new;
    io.dec_run = wfn_dec_run;
    io.dec_free = wfn_dec_free;
    io.dec_accept = a_dec_accept;
    io.dec_name = wfn_dec_name;
    e->seed = a_now(e);
    if (!(e->w = wfn_new(&io, window))) {
        wfna_free(e);
        return NULL;
    }
    return e;
}

void wfna_free(struct wfna *e)
{
    if (!e)
        return;
    wfn_free(e->w);                             /* closes its sockets and TLS connections first */
    if (e->ctx)
        OT_FreeContext(e->ctx);
    if (e->tlsbase)
        CloseLibrary(e->tlsbase);
    if (e->dtbase)
        CloseLibrary(e->dtbase);
    CloseLibrary(e->socketbase);
    FreeVec(e);
}

LONG wfna_call(struct wfna *e, UWORD op, ULONG arg, const ULONG extra[4], struct wfn_buf buf[4], ULONG *result, ULONG *aux)
{
    unsigned long ex[4], r = 0, a = 0;
    LONG st;
    int i;
    for (i = 0; i < 4; i++)
        ex[i] = extra ? extra[i] : 0;
    st = wfn_call(e->w, op, arg, ex, buf, &r, &a);
    *result = r;
    *aux = a;
    return st;
}

ULONG wfna_wait(struct wfna *e, ULONG ms, ULONG sigs)
{
    ULONG got;
    e->sigs = sigs;
    e->got = 0;
    wfn_pump(e->w, ms);
    got = e->got;
    e->sigs = e->got = 0;
    return got;
}
