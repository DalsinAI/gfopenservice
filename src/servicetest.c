/*
 * ServiceTest: tries mitchy.library (and so gfopenservice.device) end to end.
 *
 *   - the ACSV boards in this machine (Mitchy, AkikoPlus, the CPU cores
 *     board), or that none is fitted;
 *   - the services on offer;
 *   - echo/1 (or NAME): COUNT calls waited for, each answer checked and
 *     timed; for echo/1 also PARALLEL calls in flight at once, completing by
 *     signal and by message; a call cancelled while the service waits; a
 *     buffer too small; a buffer not marked written; MitchyCall;
 *   - each real service on offer, with one small job it can be checked on:
 *     input.pads/1 (INFO), log/1 (a line into the runtime's log),
 *     openweb.fetch/1 (STATS), media.encode/1 (FORMATS), opentls.key/1 (two
 *     X25519 key pairs and both sides' secret), media.decode/1 (a small SVG,
 *     PROBE and DECODE), media.cdxl/1 (a READ of no conversion, refused),
 *     akiko.swap/1 (16-bit swaps, on AkikoPlus), doc.render/1 (a text
 *     file's TEXT, unless QUICK).
 *
 * Usage: ServiceTest [NAME=<service>] [COUNT=<n>] [PARALLEL=<n>] [QUICK]
 * Returns 0 when everything passed, 5 (WARN) when no board or Cradle
 * offers the service (the "not fitted" path), 10 (ERROR) when a check
 * failed, 20 when mitchy.library is missing.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/mitchy.h>

const char version[] __attribute__((used)) = "$VER: ServiceTest 0.2 (9.11.2026)";

struct Library *MitchyBase;
static LONG fails, passes;

static void check(const char *what, int ok, const char *detail)
{
    if (ok)
        passes++;
    else
        fails++;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static LONG ticksSince(const struct DateStamp *t0)
{
    struct DateStamp t1;
    DateStamp(&t1);
    return (t1.ds_Days - t0->ds_Days) * 86400 * 50 + (t1.ds_Minute - t0->ds_Minute) * 3000 + (t1.ds_Tick - t0->ds_Tick);
}

static const char *className(ULONG klass)
{
    switch (klass) {
    case MITCHY_CLASS_SERVICES: return "Mitchy, the services card";
    case MITCHY_CLASS_CORES: return "the CPU cores board";
    case MITCHY_CLASS_FPU: return "an FPU board";
    case MITCHY_CLASS_TPU: return "a TPU board";
    case MITCHY_CLASS_MEDIA: return "AkikoPlus, the media board";
    }
    return "an ACSV board";
}

/* The boards, and how many there are. */
static int boards(void)
{
    struct ConfigDev *cd = NULL;
    int n = 0;
    while ((cd = MitchyFindBoard(cd, MITCHY_CLASS_ANY))) {
        ULONG klass = ((volatile ULONG *)cd->cd_BoardAddr)[0x20 / 4];
        printf("board: %s (CLASS %lu, product %u) at $%08lx, %lu KB\n", className(klass), (unsigned long)klass,
               (unsigned)cd->cd_Rom.er_Product, (unsigned long)cd->cd_BoardAddr, (unsigned long)(cd->cd_BoardSize >> 10));
        n++;
    }
    if (!n)
        printf("board: none. Mitchy (the services card) is not fitted in this machine\n");
    else if (!MitchyFindBoard(NULL, MITCHY_CLASS_SERVICES))
        printf("board: Mitchy (the services card) is not fitted\n");
    return n;
}

/* ---- echo/1 ----------------------------------------------------------------- */

static void fillEcho(struct MitchyRequest *r, char *in, char *out, ULONG n, ULONG room)
{
    r->mr_Op = 1;
    r->mr_Flags = MBF_BUF1;
    r->mr_Buf[0].mb_Data = in;
    r->mr_Buf[0].mb_Length = n;
    r->mr_Buf[1].mb_Data = out;
    r->mr_Buf[1].mb_Length = room;
}

static void echoSync(struct MitchyService *svc, LONG count)
{
    static char in[256], out[256];
    struct MitchyRequest *req = MitchyAllocRequest(svc, NULL);
    struct DateStamp t0;
    LONG i, bad = 0, ticks;
    char line[96];
    if (!req) {
        check("echo: a request", 0, "no memory or signal");
        return;
    }
    DateStamp(&t0);
    for (i = 0; i < count; i++) {
        int n = 1 + (int)(i % (sizeof in - 1)), k;
        LONG st;
        for (k = 0; k < n; k++)
            in[k] = (char)(i + k);
        memset(out, 0, sizeof out);
        fillEcho(req, in, out, n, sizeof out);
        st = MitchyDoRequest(req);
        if (st || req->mr_Result != (ULONG)n || memcmp(in, out, n)) {
            if (bad++ < 3)
                printf("  echo %ld: status %ld, result %lu\n", (long)i, (long)st, (unsigned long)req->mr_Result);
        }
    }
    ticks = ticksSince(&t0);
    sprintf(line, "%ld calls, %ld bad, %ld ms, %ld us each", (long)count, (long)bad, (long)ticks * 20,
            count ? (long)(ticks * 20000 / count) : 0L);
    check("echo: waited for (MitchyDoRequest)", bad == 0, line);
    MitchyFreeRequest(req);
}

/* n requests in flight at once, count calls in all; completions as messages
 * on one port (byPort) or each by its own signal. */
static void echoParallel(struct MitchyService *svc, LONG n, LONG count, int byPort)
{
    static struct MitchyRequest *reqs[8];
    static char ins[8][64], outs[8][64];
    static UBYTE out_[8];
    struct MsgPort *port = byPort ? CreateMsgPort() : NULL;
    struct DateStamp t0;
    LONG sent = 0, done = 0, bad = 0, k;
    ULONG all = 0;
    char line[96];
    if (n < 1)
        n = 1;
    if (n > 8)
        n = 8;
    if (byPort && !port)
        return;
    for (k = 0; k < n; k++) {
        if (!(reqs[k] = MitchyAllocRequest(svc, port))) {
            check("echo in parallel: requests", 0, "no memory or signal");
            n = k;
            goto out;
        }
        all |= reqs[k]->mr_SigMask;
        out_[k] = 0;
    }
    DateStamp(&t0);
    for (k = 0; k < n && sent < count; k++, sent++) {
        memset(ins[k], (int)sent, sizeof ins[k]);
        memset(outs[k], 0, sizeof outs[k]);
        fillEcho(reqs[k], ins[k], outs[k], sizeof ins[k], sizeof outs[k]);
        if (MitchySendRequest(reqs[k]))
            bad++;
        out_[k] = 1;
    }
    while (done < sent) {
        if (byPort) {
            struct Message *m;
            WaitPort(port);
            while ((m = GetMsg(port))) {
                struct MitchyRequest *r = MitchyRequestFromMsg(m);
                for (k = 0; k < n && reqs[k] != r; k++)
                    ;
                if (k == n || !MitchyCheckRequest(r)) {
                    bad++;
                    continue;
                }
                out_[k] = 0;
                done++;
                if (r->mr_Status || memcmp(ins[k], outs[k], sizeof ins[k]))
                    bad++;
                if (sent < count) {
                    memset(ins[k], (int)sent, sizeof ins[k]);
                    memset(outs[k], 0, sizeof outs[k]);
                    MitchySendRequest(r);
                    out_[k] = 1;
                    sent++;
                }
            }
        } else {
            Wait(all);
            for (k = 0; k < n; k++) {
                struct MitchyRequest *r = reqs[k];
                if (!out_[k] || !MitchyCheckRequest(r))
                    continue;
                out_[k] = 0;
                done++;
                if (r->mr_Status || memcmp(ins[k], outs[k], sizeof ins[k]))
                    bad++;
                if (sent < count) {
                    memset(ins[k], (int)sent, sizeof ins[k]);
                    memset(outs[k], 0, sizeof outs[k]);
                    MitchySendRequest(r);
                    out_[k] = 1;
                    sent++;
                }
            }
        }
    }
    sprintf(line, "%ld in flight, %ld calls, %ld bad, %ld ms", (long)n, (long)done, (long)bad, (long)ticksSince(&t0) * 20);
    check(byPort ? "echo in parallel, completions as messages on one port" : "echo in parallel, completions by signal",
          bad == 0 && done == count, line);
out:
    for (k = 0; k < n; k++)
        MitchyFreeRequest(reqs[k]);
    if (port)
        DeleteMsgPort(port);
}

static void echoEdges(struct MitchyService *svc)
{
    static char in[32], out[64];
    struct MitchyRequest *req = MitchyAllocRequest(svc, NULL);
    struct DateStamp t0;
    LONG st, ticks;
    char line[96];
    if (!req)
        return;
    /* echo/1's test hook: extra[0] ms to wait first, cut short by a cancel */
    memset(in, 'm', sizeof in);
    memset(out, 0, sizeof out);
    fillEcho(req, in, out, sizeof in, sizeof out);
    req->mr_Extra[0] = 3000;
    DateStamp(&t0);
    MitchySendRequest(req);
    Delay(10);
    check("echo: in flight 0.2 s into a 3 s wait (MitchyCheckRequest FALSE)", !MitchyCheckRequest(req), NULL);
    st = MitchyAbortRequest(req);
    ticks = ticksSince(&t0);
    sprintf(line, "status %ld (%s), %ld ms from the send, buffer %s", (long)st, (const char *)MitchyStatusText(st),
            (long)ticks * 20, out[0] ? "written" : "untouched");
    check("echo: cancelled (MitchyAbortRequest)", st == MERR_CANCELLED && ticks < 100 && !out[0], line);
    req->mr_Extra[0] = 0;
    st = MitchyDoRequest(req);
    check("echo: the same request again after the cancel", st == 0 && !memcmp(in, out, sizeof in), NULL);

    req->mr_Buf[1].mb_Length = 8;
    st = MitchyDoRequest(req);
    sprintf(line, "status %ld, result %lu", (long)st, (unsigned long)req->mr_Result);
    check("echo: buffer too small: -4, result the room needed", st == MERR_TOOSMALL && req->mr_Result == sizeof in, line);
    req->mr_Buf[1].mb_Length = sizeof out;
    req->mr_Flags = 0;
    st = MitchyDoRequest(req);
    sprintf(line, "status %ld", (long)st);
    check("echo: buffer 1 not marked written: -2", st == MERR_BADREQUEST, line);
    {
        ULONG ra[2] = { 0, 0 };
        struct MitchyBuffer b[4];
        memset(b, 0, sizeof b);
        memset(out, 0, sizeof out);
        b[0].mb_Data = in;
        b[0].mb_Length = 5;
        b[1].mb_Data = out;
        b[1].mb_Length = sizeof out;
        st = MitchyCall(svc, 1, MBF_BUF1, 0, b, NULL, ra);
        check("echo: MitchyCall", st == 0 && ra[0] == 5 && !memcmp(in, out, 5), NULL);
    }
    MitchyFreeRequest(req);
}

/* ---- the real services --------------------------------------------------------- */

static int offered(const char *list, LONG length, const char *name)
{
    LONG i;
    for (i = 0; i < length; i += strlen(list + i) + 1)
        if (!strcmp(list + i, name))
            return 1;
    return 0;
}

static struct MitchyService *openOne(const char *name)
{
    LONG err;
    struct MitchyService *svc = MitchyOpen((CONST_STRPTR)name, 0, &err);
    if (!svc) {
        check(name, 0, (const char *)MitchyStatusText(err));
        return NULL;
    }
    printf("open %s: %s\n", name, svc->ms_Where == MITCHY_WHERE_BOARD ? "on a board" : "on a Cradle on the LAN");
    return svc;
}

static ULONG be32(const UBYTE *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }

static void testPads(void)
{
    struct MitchyService *svc = openOne("input.pads/1");
    ULONG ra[2];
    LONG st;
    char line[64];
    if (!svc)
        return;
    st = MitchyCall(svc, 1, 0, 0, NULL, NULL, ra);
    sprintf(line, "window at $%lx, version %lu", (unsigned long)ra[0], (unsigned long)ra[1]);
    check("input.pads/1 INFO", st == 0 && ra[0] && ra[1] >= 1, line);
    MitchyClose(svc);
}

static void testLog(void)
{
    static const char text[] = "ServiceTest 0.2: mitchy.library says hello";
    struct MitchyService *svc = openOne("log/1");
    struct MitchyBuffer b[4];
    ULONG ra[2];
    LONG st;
    if (!svc)
        return;
    memset(b, 0, sizeof b);
    b[0].mb_Data = (APTR)text;
    b[0].mb_Length = sizeof text - 1;
    st = MitchyCall(svc, 1, 0, 0, b, NULL, ra);
    check("log/1 LINE (\"Amiga: ServiceTest 0.2: ...\" in the runtime's log)", st == 0 && ra[0] == sizeof text - 1, NULL);
    MitchyClose(svc);
}

static void testWeb(void)
{
    struct MitchyService *svc = openOne("openweb.fetch/1");
    ULONG ra[2];
    LONG st;
    char line[64];
    if (!svc)
        return;
    st = MitchyCall(svc, 7, 0, 0, NULL, NULL, ra);
    sprintf(line, "%lu fetches open, %lu threads", (unsigned long)ra[0], (unsigned long)ra[1]);
    check("openweb.fetch/1 STATS", st == 0, line);
    MitchyClose(svc);
}

static void testEncode(void)
{
    static UBYTE formats[64];
    struct MitchyService *svc = openOne("media.encode/1");
    struct MitchyBuffer b[4];
    ULONG ra[2], i;
    LONG st;
    char line[96];
    int n;
    if (!svc)
        return;
    memset(b, 0, sizeof b);
    b[1].mb_Data = formats;
    b[1].mb_Length = sizeof formats;
    st = MitchyCall(svc, 16, MBF_BUF1, 0, b, NULL, ra);
    n = sprintf(line, "status %ld, %lu:", (long)st, (unsigned long)ra[0]);
    for (i = 0; st == 0 && i < ra[0] && i < 12; i++) {
        ULONG f = be32(formats + 4 * i);
        n += sprintf(line + n, " %c%c%c%c", (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f);
    }
    check("media.encode/1 FORMATS", st == 0 && ra[0] >= 1, line);
    MitchyClose(svc);
}

static void testTls(void)
{
    static UBYTE aPriv[64], aPub[64], bPriv[64], bPub[64], s1[64], s2[64];
    struct MitchyService *svc = openOne("opentls.key/1");
    struct MitchyBuffer b[4];
    ULONG ra[2], la, lb;
    LONG st1, st2, st3, st4;
    char line[100];
    if (!svc)
        return;
    memset(b, 0, sizeof b);
    b[0].mb_Data = aPriv; b[0].mb_Length = sizeof aPriv;
    b[1].mb_Data = aPub; b[1].mb_Length = sizeof aPub;
    st1 = MitchyCall(svc, 2, MBF_BUF0 | MBF_BUF1, 1, b, NULL, ra);       /* KEYGEN X25519 */
    la = ra[0];
    b[0].mb_Data = bPriv; b[1].mb_Data = bPub;
    st2 = MitchyCall(svc, 2, MBF_BUF0 | MBF_BUF1, 1, b, NULL, ra);
    lb = ra[0];
    b[0].mb_Data = aPriv; b[0].mb_Length = 32;
    b[1].mb_Data = bPub; b[1].mb_Length = lb;
    b[2].mb_Data = s1; b[2].mb_Length = sizeof s1;
    st3 = MitchyCall(svc, 3, MBF_BUF2, 1, b, NULL, ra);                  /* DERIVE, each side */
    b[0].mb_Data = bPriv;
    b[1].mb_Data = aPub; b[1].mb_Length = la;
    b[2].mb_Data = s2;
    st4 = MitchyCall(svc, 3, MBF_BUF2, 1, b, NULL, ra);
    sprintf(line, "statuses %ld %ld %ld %ld, public keys %lu and %lu bytes, secret %lu", (long)st1, (long)st2, (long)st3,
            (long)st4, (unsigned long)la, (unsigned long)lb, (unsigned long)ra[0]);
    check("opentls.key/1 X25519: two key pairs, the same secret both ways",
          !st1 && !st2 && !st3 && !st4 && la == 32 && lb == 32 && ra[0] == 32 && !memcmp(s1, s2, 32) && memcmp(aPub, bPub, 32), line);
    MitchyClose(svc);
}

static const char svg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"30\">"
    "<rect width=\"40\" height=\"30\" fill=\"#ff0000\"/></svg>\n";

static void testDecode(void)
{
    static UBYTE info[24], pixels[40 * 30 * 4];
    struct MitchyService *svc = openOne("media.decode/1");
    struct MitchyBuffer b[4];
    ULONG ra[2], x[4] = { 0, 0, 0x53564720UL, 0 };   /* hint 'SVG ' */
    LONG st;
    char line[96];
    if (!svc)
        return;
    memset(b, 0, sizeof b);
    b[0].mb_Data = (APTR)svg; b[0].mb_Length = sizeof svg - 1;
    b[1].mb_Data = info; b[1].mb_Length = sizeof info;
    st = MitchyCall(svc, 1, MBF_BUF1, 0, b, x, ra);
    sprintf(line, "status %ld, %lu x %lu, format %.4s", (long)st, (unsigned long)ra[0], (unsigned long)ra[1], (char *)info + 4);
    check("media.decode/1 PROBE of an SVG", st == 0 && ra[0] == 40 && ra[1] == 30 && be32(info) == 1, line);
    b[1].mb_Data = pixels; b[1].mb_Length = sizeof pixels;
    st = MitchyCall(svc, 2, MBF_BUF1, 0, b, x, ra);
    sprintf(line, "status %ld, %lu x %lu, first pixel %08lx", (long)st, (unsigned long)ra[0], (unsigned long)ra[1],
            (unsigned long)be32(pixels));
    check("media.decode/1 DECODE: red ARGB pixels", st == 0 && ra[0] == 40 && ra[1] == 30 && be32(pixels) == 0xffff0000UL &&
          be32(pixels + sizeof pixels - 4) == 0xffff0000UL, line);
    MitchyClose(svc);
}

static void testCdxl(void)
{
    static UBYTE piece[16];
    struct MitchyService *svc = openOne("media.cdxl/1");
    struct MitchyBuffer b[4];
    ULONG ra[2];
    LONG st;
    char line[64];
    if (!svc)
        return;
    memset(b, 0, sizeof b);
    b[1].mb_Data = piece; b[1].mb_Length = sizeof piece;
    st = MitchyCall(svc, 2, MBF_BUF1, 0x7fffffffUL, b, NULL, ra);    /* READ of a conversion nobody made */
    sprintf(line, "status %ld (%s)", (long)st, (const char *)MitchyStatusText(st));
    check("media.cdxl/1 answers (a READ of no conversion is refused)", st < 0 && st != MERR_NOSERVICE && st != MERR_LOST, line);
    MitchyClose(svc);
}

static void testSwap(void)
{
    static UBYTE in[16], out[16];
    struct MitchyService *svc = openOne("akiko.swap/1");
    struct MitchyBuffer b[4];
    ULONG ra[2];
    LONG st, i;
    int ok;
    if (!svc)
        return;
    for (i = 0; i < 16; i++)
        in[i] = (UBYTE)i;
    memset(out, 0, sizeof out);
    memset(b, 0, sizeof b);
    b[0].mb_Data = in; b[0].mb_Length = 16;
    b[1].mb_Data = out; b[1].mb_Length = 16;
    st = MitchyCall(svc, 1, MBF_BUF1, 0, b, NULL, ra);
    ok = st == 0;
    for (i = 0; ok && i < 16; i++)
        ok = out[i] == in[i ^ 1];
    check("akiko.swap/1 16-bit swaps (AkikoPlus)", ok, NULL);
    MitchyClose(svc);
}

static void testDoc(void)
{
    static const char text[] = "Hello from Mitchy\n";
    static char out[4096];
    struct MitchyService *svc = openOne("doc.render/1");
    struct MitchyRequest *req;
    LONG st, waited = 0;
    char line[96];
    if (!svc)
        return;
    if (!(req = MitchyAllocRequest(svc, NULL))) {
        MitchyClose(svc);
        return;
    }
    memset(out, 0, sizeof out);
    req->mr_Op = 3;                                   /* TEXT */
    req->mr_Flags = MBF_BUF1;
    req->mr_Buf[0].mb_Data = (APTR)text; req->mr_Buf[0].mb_Length = sizeof text - 1;
    req->mr_Buf[1].mb_Data = out; req->mr_Buf[1].mb_Length = sizeof out - 1;
    req->mr_Extra[2] = 0x54585420UL;                  /* 'TXT ' */
    MitchySendRequest(req);
    /* the office may take a while to start: 90 s at most, then cancel */
    while (!MitchyCheckRequest(req) && waited < 90 * 50) {
        Delay(10);
        waited += 10;
    }
    st = MitchyCheckRequest(req) ? req->mr_Status : MitchyAbortRequest(req);
    sprintf(line, "status %ld (%s), %lu bytes, %ld s", (long)st, (const char *)MitchyStatusText(st),
            (unsigned long)req->mr_Result, (long)waited / 50);
    if (st == MERR_HOST)                              /* the service answered: the host has no office (or it failed) */
        printf("skip doc.render/1 TEXT: %s (no LibreOffice or OpenOffice on the host?)\n", line);
    else
        check("doc.render/1 TEXT of a text file (async, polled)", st == 0 && strstr(out, "Hello from Mitchy") != NULL, line);
    MitchyFreeRequest(req);
    MitchyClose(svc);
}

int main(void)
{
    LONG args[4] = { 0, 0, 0, 0 };
    struct RDArgs *rd = ReadArgs((CONST_STRPTR)"NAME,COUNT/N,PARALLEL/N,QUICK/S", args, NULL);
    const char *name = args[0] ? (const char *)args[0] : "echo/1";
    LONG count = args[1] ? *(LONG *)args[1] : 100, err, length;
    LONG parallel = args[2] ? *(LONG *)args[2] : 4;
    static char list[1024];
    struct MitchyService *svc;
    int nboards, rc;
    if (!rd) {
        printf("ServiceTest: bad arguments (NAME,COUNT/N,PARALLEL/N,QUICK/S)\n");
        return RETURN_ERROR;
    }
    if (!(MitchyBase = OpenLibrary((CONST_STRPTR)MITCHYNAME, MITCHYVERSION))) {
        printf("ServiceTest: cannot open %s %d (LIBS:)\n", MITCHYNAME, MITCHYVERSION);
        FreeArgs(rd);
        return RETURN_FAIL;
    }
    printf("%s", (const char *)MitchyBase->lib_IdString);
    printf("\n");
    nboards = boards();

    length = MitchyList((STRPTR)list, sizeof list);
    if (length < 0)
        printf("list: %s\n", (const char *)MitchyStatusText(length));
    else {
        LONG i;
        printf("list:");
        for (i = 0; i < length; i += strlen(list + i) + 1)
            printf(" %s", list + i);
        printf("%s\n", length ? "" : " (none)");
    }

    svc = MitchyOpen((CONST_STRPTR)name, 0, &err);
    if (!svc) {
        printf("open %s: %s\n", name, (const char *)MitchyStatusText(err));
        if (err == MERR_NOTFITTED || (err == MERR_NOSERVICE && !nboards))
            printf("not fitted: no services card here and no paired Cradle offers it; programs do this work on the 68k\n");
        CloseLibrary(MitchyBase);
        FreeArgs(rd);
        return err == MERR_NOTFITTED || err == MERR_NOSERVICE ? RETURN_WARN : RETURN_ERROR;
    }
    printf("open %s: handle %u, %s\n", name, (unsigned)svc->ms_Handle,
           MitchyWhere(svc) == MITCHY_WHERE_BOARD ? "on a board in this machine" : "on a Cradle on the LAN");
    {
        LONG e2;
        struct MitchyService *none = MitchyOpen((CONST_STRPTR)"no.such.service/1", 0, &e2);
        check("open of a name nobody offers: MERR_NOSERVICE", !none && e2 == MERR_NOSERVICE, NULL);
        if (none)
            MitchyClose(none);
        if (nboards) {
            struct MitchyService *local = MitchyOpen((CONST_STRPTR)name, MOF_BOARDONLY, &e2);
            check("open with MOF_BOARDONLY: on a board", local && local->ms_Where == MITCHY_WHERE_BOARD, NULL);
            if (local)
                MitchyClose(local);
        }
    }
    echoSync(svc, count);
    if (!strcmp(name, "echo/1")) {
        echoParallel(svc, parallel, count, 0);
        echoParallel(svc, parallel, count, 1);
        echoEdges(svc);
    }
    MitchyClose(svc);

    if (length > 0) {
        if (offered(list, length, "input.pads/1")) testPads();
        if (offered(list, length, "log/1")) testLog();
        if (offered(list, length, "openweb.fetch/1")) testWeb();
        if (offered(list, length, "media.encode/1")) testEncode();
        if (offered(list, length, "opentls.key/1")) testTls();
        if (offered(list, length, "media.decode/1")) testDecode();
        if (offered(list, length, "media.cdxl/1")) testCdxl();
        if (offered(list, length, "akiko.swap/1")) testSwap();
        if (offered(list, length, "doc.render/1")) {
            if (args[3])
                printf("skip doc.render/1 (QUICK)\n");
            else
                testDoc();
        }
    }
    printf("ServiceTest: %ld passed, %ld failed\n", (long)passes, (long)fails);
    rc = fails ? RETURN_ERROR : RETURN_OK;
    CloseLibrary(MitchyBase);
    FreeArgs(rd);
    return rc;
}
