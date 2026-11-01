/*
 * openservice.device (devices/openservice.h).
 *
 * One worker process does everything: it owns the services card's rings and
 * the LAN connections, so nothing here needs a lock beyond Forbid() around
 * the device's own open count. BeginIO hands each request to the worker's
 * port; the worker writes it to the card's submission ring, or sends it to
 * a Cradle, and replies when the completion comes back.
 *
 * The card's wire is design/SERVICES_CARD.md (v1). Over the LAN each frame
 * is the same 64-byte entry, then the buffers the service reads; the answer
 * is the 16-byte completion, then for each buffer the service writes a u32
 * byte count and that many bytes.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#include <string.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>

#include "devices/openservice.h"
#include "openservice.h"
#include "mdns.h"
#include "oscrypto.h"
#include "osrandom.h"

#ifndef REG
#define REG(reg, arg) arg __asm(#reg)
#endif

const char DevName[] = OPENSERVICE_NAME;
/* "Version DEVS:openservice.device" reads this; the ROMTag shows the rest. */
const char DevVersion[] __attribute__((used)) = "$VER: openservice.device 1.1 (6.10.2026)";
#define DevIdString (DevVersion + 6)
#define DEV_VERSION 1
#define DEV_REVISION 1

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct ExpansionBase *ExpansionBase;
struct Library *SocketBase;          /* the worker's own */

static BPTR segList;
static struct Process *worker;
static struct MsgPort *workerPort;   /* the worker's: requests arrive here */
static BYTE cardSignal = -1, abortSignal = -1, quitSignal = -1;
static struct Task *starter;         /* waiting for the worker to start or stop */

/* ---- the card ------------------------------------------------------------- */

#define REG_MAGIC    0x00
#define REG_VERSION  0x04
#define REG_RINGS    0x08
#define REG_ORDER    0x0c
#define REG_DOORBELL 0x10
#define REG_INTENA   0x14
#define REG_INTREQ   0x18

#define RING_ORDER 6
#define RING_N (1u << RING_ORDER)
#define RING_BYTES (0x100 + RING_N * 64 + RING_N * 16)

#define DIR_OPEN   1
#define DIR_CLOSE  2
#define DIR_LIST   3
#define OP_CANCEL  0xffff

#define REG_CLASS    0x20

/* Every AmigaChrome Team board with the services block: the services card, CPU cores,
 * FPU or TPU boards. Each has its own rings and interrupt server; the
 * directory on each says what it offers. */
#define MAX_BOARDS 4
struct board {
    volatile ULONG *regs;
    UBYTE *memory, *ring;            /* ring is memory 4 KB aligned */
    ULONG sqTail, cqHead;
    struct Interrupt irq;
};
static struct board boards[MAX_BOARDS];
static int boardCount;

#define RING_U32(b, off) (*(volatile ULONG *)((b)->ring + (off)))
#define SQ_TAIL(b) RING_U32(b, 0x000)
#define SQ_HEAD(b) RING_U32(b, 0x040)
#define CQ_TAIL(b) RING_U32(b, 0x080)
#define CQ_HEAD(b) RING_U32(b, 0x0c0)
#define SQ_ENTRY(b, i) ((b)->ring + 0x100 + ((i) & (RING_N - 1)) * 64)
#define CQ_ENTRY(b, i) ((b)->ring + 0x100 + RING_N * 64 + ((i) & (RING_N - 1)) * 16)

/* Only the data cache, and only the rings: CacheClearU also clears the
 * instruction cache, which makes AC090 throw away its translated code. */
#define RING_FLUSH(b) CacheClearE((b)->ring, RING_BYTES, CACRF_ClearD)

static ULONG boardServer(REG(a1, struct board *b))
{
    if (b->regs[REG_INTREQ / 4] & 1) {
        b->regs[REG_INTREQ / 4] = 1;  /* clear first: a later completion raises it again */
        Signal(&worker->pr_Task, 1UL << cardSignal);
    }
    return 0;                         /* the PORTS chain is shared: let the others look */
}

static void cardStart(void)
{
    struct ConfigDev *cd = NULL;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 37);
    if (!ExpansionBase)
        return;
    while (boardCount < MAX_BOARDS && (cd = FindConfigDev(cd, OPENSERVICE_MANUFACTURER, -1))) {
        volatile ULONG *regs = (volatile ULONG *)cd->cd_BoardAddr;
        struct board *b = &boards[boardCount];
        if ((cd->cd_Rom.er_Type & ERTF_MEMLIST) || (cd->cd_Flags & CDF_SHUTUP)
            || regs[REG_MAGIC / 4] != OPENSERVICE_MAGIC || regs[REG_VERSION / 4] != 1)
            continue;
        b->memory = AllocMem(RING_BYTES + 4096, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
        if (!b->memory)
            break;
        b->ring = (UBYTE *)(((ULONG)b->memory + 4095) & ~4095UL);
        b->regs = regs;
        b->sqTail = b->cqHead = 0;
        b->irq.is_Node.ln_Type = NT_INTERRUPT;
        b->irq.is_Node.ln_Name = (char *)DevName;
        b->irq.is_Data = b;
        b->irq.is_Code = (void (*)(void))boardServer;
        AddIntServer(INTB_PORTS, &b->irq);
        RING_FLUSH(b);
        regs[REG_ORDER / 4] = RING_ORDER;
        regs[REG_RINGS / 4] = (ULONG)b->ring;
        regs[REG_INTENA / 4] = 1;
        boardCount++;
    }
    CloseLibrary((struct Library *)ExpansionBase);
    ExpansionBase = NULL;
}

static void cardStop(void)
{
    while (boardCount) {
        struct board *b = &boards[--boardCount];
        b->regs[REG_INTENA / 4] = 0;
        b->regs[REG_RINGS / 4] = 0;
        RemIntServer(INTB_PORTS, &b->irq);
        FreeMem(b->memory, RING_BYTES + 4096);
        b->regs = NULL;
    }
}

/* ---- what is in flight ---------------------------------------------------- */

enum { WHERE_NONE, WHERE_CARD = OSWHERE_CARD, WHERE_LAN = OSWHERE_LAN };
enum { KIND_USER, KIND_OPEN, KIND_LIST, KIND_INTERNAL, KIND_JOIN };

#define MAX_PENDING 64
#define MAX_HANDLES 32
#define MAX_CONNS 4

struct pending {
    struct OSRequest *io;            /* NULL: free, or internal */
    ULONG id;
    UBYTE kind, where, conn, cancelSent;
    UBYTE handle;                    /* KIND_JOIN: the handle it joins */
};
static struct pending pending[MAX_PENDING];
static ULONG nextId = 1;

/* A LAN handle can be open on several Cradles (a cluster): each call goes
 * to the one with the fewest requests in flight. */
struct handle {
    UBYTE where, conn;
    UWORD remote;
    UBYTE more;                      /* other Cradles it is open on */
    UBYTE moreConn[MAX_CONNS];
    UWORD moreRemote[MAX_CONNS];
};
static struct handle handles[MAX_HANDLES];   /* local handle = index + 1 */

struct conn {
    LONG socket;                     /* -1: closed */
    ULONG address;
    UWORD port;
    UWORD users;                     /* handles on it */
    /* A sealed session (docs/PAIRING.md): keys each way, the record
     * counters, and the record being read. */
    UBYTE sealed;
    UBYTE txKey[32], rxKey[32];
    ULONG txCount, rxCount;
    UBYTE *record;
    ULONG recordLength, recordAt;
};
static struct conn conns[MAX_CONNS];

static struct mdns_cradle cradles[8];
static int cradleCount;
static ULONG browsedAt;              /* seconds, 0: never */

static struct List waiting;          /* requests the card ring had no room for */
/* With ENV:OpenService/Debug present, the worker logs to T:openservice.log. */
static BPTR debugLog;
static void dbg(const char *what, ULONG a, ULONG b)
{
    static const char hex[] = "0123456789abcdef";
    char line[96];
    int n = 0, k;
    if (!debugLog)
        return;
    while (*what && n < 60)
        line[n++] = *what++;
    for (k = 0; k < 2; k++) {
        ULONG v = k ? b : a;
        int d;
        line[n++] = ' ';
        for (d = 28; d >= 0; d -= 4)
            line[n++] = hex[(v >> d) & 15];
    }
    line[n++] = '\n';
    Write(debugLog, line, n);
}


static struct pending *newPending(struct OSRequest *io, int kind, int where, int conn)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++)
        if (!pending[i].id) {
            struct pending *p = &pending[i];
            p->io = io;
            p->id = nextId++;
            if (!nextId)
                nextId = 1;
            p->kind = (UBYTE)kind;
            p->where = (UBYTE)where;
            p->conn = (UBYTE)conn;
            p->cancelSent = 0;
            return p;
        }
    return NULL;
}

static struct pending *findPending(ULONG id)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++)
        if (pending[i].id == id)
            return &pending[i];
    return NULL;
}

static void finish(struct OSRequest *io, LONG status, ULONG result, ULONG aux)
{
    io->os_Status = status;
    io->os_Result = result;
    io->os_Aux = aux;
    io->os_Req.io_Error = status == OSERR_CANCELLED ? IOERR_ABORTED : 0;
    ReplyMsg(&io->os_Req.io_Message);
}

/* The 64-byte entry for io, as the card and the LAN both take it. */
static void putEntry(UBYTE *e, ULONG id, UWORD service, UWORD op, const struct OSRequest *io)
{
    ULONG *l = (ULONG *)e;
    int i;
    l[0] = id;
    ((UWORD *)e)[2] = service;
    ((UWORD *)e)[3] = op;
    l[2] = io ? io->os_Flags : 0;
    l[3] = io ? io->os_Arg : 0;
    for (i = 0; i < 4; i++) {
        l[4 + 2 * i] = io ? (ULONG)io->os_Buf[i].ob_Data : 0;
        l[5 + 2 * i] = io ? io->os_Buf[i].ob_Length : 0;
    }
    for (i = 0; i < 4; i++)
        l[12 + i] = io ? io->os_Extra[i] : 0;
}

/* ---- the card path ---------------------------------------------------------- */

static int cardSubmit(struct pending *p, UWORD service, UWORD op, const struct OSRequest *io, ULONG arg)
{
    struct board *b = &boards[p->conn];
    UBYTE *e;
    int i;
    if (b->sqTail - SQ_HEAD(b) >= RING_N)
        return 0;
    e = SQ_ENTRY(b, b->sqTail);
    putEntry(e, p->id, service, op, io);
    if (!io)
        ((ULONG *)e)[3] = arg;
    /* The buffers out of the data cache before the host reads or writes
     * them. Not CachePreDMA/CachePostDMA: on AC090 each pair costs about
     * 250 ms, where this costs well under a millisecond. */
    for (i = 0; io && i < 4; i++)
        if (io->os_Buf[i].ob_Length)
            CacheClearE(io->os_Buf[i].ob_Data, io->os_Buf[i].ob_Length, CACRF_ClearD);
    b->sqTail++;
    SQ_TAIL(b) = b->sqTail;
    RING_FLUSH(b);
    b->regs[REG_DOORBELL / 4] = 1;
    return 1;
}

static void completed(struct pending *p, LONG status, ULONG result, ULONG aux);
static void listNext(struct OSRequest *io);

static void cardDrain(void)
{
    int k;
    for (k = 0; k < boardCount; k++) {
        struct board *b = &boards[k];
        ULONG tail;
        RING_FLUSH(b);
        while (b->cqHead != (tail = CQ_TAIL(b))) {
            while (b->cqHead != tail) {
                ULONG *c = (ULONG *)CQ_ENTRY(b, b->cqHead);
                struct pending *p = findPending(c[0]);
                LONG status = (LONG)c[1];
                ULONG result = c[2], aux = c[3];
                b->cqHead++;
                if (p && p->where == WHERE_CARD && p->conn == k)
                    completed(p, status, result, aux);
            }
            CQ_HEAD(b) = b->cqHead;
            RING_FLUSH(b);
        }
    }
}

/* ---- the LAN path ------------------------------------------------------------ */

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

#define MAX_RECORD (16UL << 20)

static void nonceFor(UBYTE nonce[12], ULONG count)
{
    memset(nonce, 0, 12);
    nonce[8] = (UBYTE)(count >> 24);
    nonce[9] = (UBYTE)(count >> 16);
    nonce[10] = (UBYTE)(count >> 8);
    nonce[11] = (UBYTE)count;
}

/* Reads from connection c: straight off the socket, or out of its sealed
 * records, opening the next one as needed. */
static int connRead(int c, void *data, ULONG length)
{
    struct conn *k = &conns[c];
    UBYTE *out = data;
    if (!k->sealed)
        return recvAll(k->socket, data, length);
    while (length) {
        ULONG n;
        if (k->recordAt == k->recordLength) {
            UBYTE head[4], nonce[12];
            ULONG size;
            if (!recvAll(k->socket, head, 4))
                return 0;
            size = (ULONG)head[0] << 24 | (ULONG)head[1] << 16 | (ULONG)head[2] << 8 | head[3];
            if (size < 16 || size > MAX_RECORD)
                return 0;
            if (k->record)
                FreeVec(k->record);
            k->record = AllocVec(size, MEMF_ANY);
            k->recordLength = k->recordAt = 0;
            if (!k->record || !recvAll(k->socket, k->record, size))
                return 0;
            nonceFor(nonce, k->rxCount++);
            if (!os_aead_open(k->rxKey, nonce, head, 4, k->record, size - 16, k->record + size - 16))
                return 0;                     /* not from the paired Cradle */
            k->recordLength = size - 16;
        }
        n = k->recordLength - k->recordAt;
        if (n > length)
            n = length;
        memcpy(out, k->record + k->recordAt, n);
        k->recordAt += n;
        out += n;
        length -= n;
    }
    return 1;
}

/* Sends one frame (pieces in order) as one sealed record, or as it is. */
static int connWrite(int c, const UBYTE *const *pieces, const ULONG *lengths, int count)
{
    struct conn *k = &conns[c];
    ULONG total = 0, at = 4;
    UBYTE *record, nonce[12];
    int i, ok;
    if (!k->sealed) {
        for (i = 0; i < count; i++)
            if (lengths[i] && !sendAll(k->socket, pieces[i], lengths[i]))
                return 0;
        return 1;
    }
    for (i = 0; i < count; i++)
        total += lengths[i];
    if (total + 16 > MAX_RECORD || !(record = AllocVec(4 + total + 16, MEMF_ANY)))
        return 0;
    record[0] = (UBYTE)((total + 16) >> 24);
    record[1] = (UBYTE)((total + 16) >> 16);
    record[2] = (UBYTE)((total + 16) >> 8);
    record[3] = (UBYTE)(total + 16);
    for (i = 0; i < count; i++) {
        memcpy(record + at, pieces[i], lengths[i]);
        at += lengths[i];
    }
    nonceFor(nonce, k->txCount++);
    os_aead_seal(k->txKey, nonce, record, 4, record + 4, total, record + 4 + total);
    ok = sendAll(k->socket, record, 4 + total + 16);
    FreeVec(record);
    return ok;
}

/* The session handshake on a fresh connection to a Cradle paired with a
 * code (its key in hand): 0 when the Cradle refuses or answers wrongly. */
static int connSeal(int c, const UBYTE psk[32])
{
    struct conn *k = &conns[c];
    UBYTE hello[36], answer[20], salt[32], keys[64];
    if (!openservice_amiga_id(hello + 4))
        return 0;
    memcpy(hello, "OSE1", 4);
    os_random(hello + 20, 16, 0);
    if (!sendAll(k->socket, hello, sizeof hello) || !recvAll(k->socket, answer, 4) || memcmp(answer, "OSE1", 4)
        || !recvAll(k->socket, answer + 4, 16))
        return 0;
    memcpy(salt, hello + 20, 16);
    memcpy(salt + 16, answer + 4, 16);
    os_hkdf_sha256(psk, 32, salt, 32, "openservice session v1", keys, 64);
    memcpy(k->txKey, keys, 32);
    memcpy(k->rxKey, keys + 32, 32);
    memset(keys, 0, sizeof keys);
    k->txCount = k->rxCount = 0;
    k->record = NULL;
    k->recordLength = k->recordAt = 0;
    k->sealed = 1;
    return 1;
}

static void connLost(int c);

static int lanSubmit(struct pending *p, int c, UWORD service, UWORD op, const struct OSRequest *io, ULONG arg)
{
    UBYTE e[64];
    int i;
    const UBYTE *pieces[5];
    ULONG lengths[5];
    int count = 1;
    putEntry(e, p->id, service, op, io);
    if (!io)
        ((ULONG *)e)[3] = arg;
    pieces[0] = e;
    lengths[0] = sizeof e;
    for (i = 0; io && i < 4; i++)
        if (io->os_Buf[i].ob_Length && !(io->os_Flags & (1UL << i))) {
            pieces[count] = io->os_Buf[i].ob_Data;
            lengths[count++] = io->os_Buf[i].ob_Length;
        }
    return connWrite(c, pieces, lengths, count);
}

/* One answer from connection c: the completion, then the written buffers. */
static void lanReceive(int c)
{
    ULONG head[4];
    struct pending *p;
    int i;
    dbg("lanReceive", c, conns[c].socket);
    if (!connRead(c, head, sizeof head)) {
        connLost(c);
        return;
    }
    p = findPending(head[0]);
    for (i = 0; i < 4; i++) {
        struct OSRequest *io = p ? p->io : NULL;
        ULONG want, room, n;
        if (!io || !io->os_Buf[i].ob_Length || !(io->os_Flags & (1UL << i)))
            continue;
        if (!connRead(c, &want, 4)) {
            connLost(c);
            return;
        }
        room = io->os_Buf[i].ob_Length;
        n = want < room ? want : room;
        if (!connRead(c, io->os_Buf[i].ob_Data, n)) {
            connLost(c);
            return;
        }
        for (; n < want; n++) {          /* more than fits: read and drop */
            UBYTE drop;
            if (!connRead(c, &drop, 1)) {
                connLost(c);
                return;
            }
        }
    }
    dbg("lan answer id/status", head[0], head[1]);
    conns[c].recordAt = conns[c].recordLength;   /* the rest of a sealed answer, if nobody wanted it */
    if (p)
        completed(p, (LONG)head[1], head[2], head[3]);
}

static int connect3s(ULONG address, UWORD port)
{
    struct sockaddr_in to;
    LONG s = socket(AF_INET, SOCK_STREAM, 0), one = 1, zero = 0, err = 0;
    socklen_t errLength = sizeof err;
    fd_set ready;
    struct timeval wait;
    if (s < 0)
        return -1;
    memset(&to, 0, sizeof to);
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = port;
    to.sin_addr.s_addr = address;
    IoctlSocket(s, FIONBIO, (char *)&one);
    if (connect(s, (struct sockaddr *)&to, sizeof to) < 0) {
        FD_ZERO(&ready);
        FD_SET(s, &ready);
        wait.tv_sec = 3;
        wait.tv_usec = 0;
        if (WaitSelect(s + 1, NULL, &ready, NULL, &wait, NULL) <= 0
            || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errLength) < 0 || err) {
            CloseSocket(s);
            return -1;
        }
    }
    IoctlSocket(s, FIONBIO, (char *)&zero);
    setsockopt(s, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
    return s;
}

static int lanConnect(const struct mdns_cradle *cr)
{
    int c, free = -1;
    for (c = 0; c < MAX_CONNS; c++) {
        if (conns[c].socket >= 0 && conns[c].address == cr->address && conns[c].port == cr->port)
            return c;
        if (conns[c].socket < 0 && free < 0)
            free = c;
    }
    if (free < 0)
        return -1;
    conns[free].socket = connect3s(cr->address, cr->port);
    if (conns[free].socket < 0)
        return -1;
    conns[free].address = cr->address;
    conns[free].port = cr->port;
    conns[free].users = 0;
    conns[free].sealed = 0;
    conns[free].record = NULL;
    {
        UBYTE psk[32];
        int keyed = openservice_psk(cr->fingerprint, psk), ok = !keyed || connSeal(free, psk);
        dbg("lanConnect keyed/ok", keyed, ok);
        memset(psk, 0, sizeof psk);
        if (!ok) {                       /* paired with a code, but the Cradle disagrees */
            CloseSocket(conns[free].socket);
            conns[free].socket = -1;
            return -1;
        }
    }
    return free;
}

/* The connection to a paired Cradle offering name, opened when needed. */
static int lanFor(const char *name)
{
    struct DateStamp ds;
    ULONG now;
    int i, c;
    if (!SocketBase)
        return -1;
    {
        /* No Cradle paired: nothing on the LAN to use, so no question asked. */
        BPTR paired = Lock((CONST_STRPTR)OPENSERVICE_PAIRED_FILE, ACCESS_READ);
        if (!paired)
            return -1;
        UnLock(paired);
    }
    /* A paired Cradle offering name that is connected already: use it with
     * no question on the LAN, so an OPEN waits on a browse only when no
     * connection serves the name and the last browse is old. */
    for (i = 0; i < cradleCount; i++) {
        struct mdns_cradle *cr = &cradles[i];
        if (!cr->address || !mdns_offers(cr, name) || !openservice_paired(cr->fingerprint))
            continue;
        for (c = 0; c < MAX_CONNS; c++)
            if (conns[c].socket >= 0 && conns[c].address == cr->address && conns[c].port == cr->port)
                return c;
    }
    DateStamp(&ds);
    now = (ULONG)ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
    if (!browsedAt || now - browsedAt > 30) {
        cradleCount = mdns_browse(1, cradles, 8, 1UL << quitSignal);
        if (cradleCount < 0)
            cradleCount = 0;
        browsedAt = now ? now : 1;
    }
    for (i = 0; i < cradleCount; i++) {
        struct mdns_cradle *cr = &cradles[i];
        if (!cr->address || !mdns_offers(cr, name) || !openservice_paired(cr->fingerprint))
            continue;
        if ((c = lanConnect(cr)) >= 0)
            return c;
    }
    return -1;
}

/* After a handle opened on one Cradle: open it on every other paired Cradle
 * offering the service too (KIND_JOIN), so calls can be spread. io carries
 * the name and is only read here. */
static int lanSubmit(struct pending *p, int c, UWORD service, UWORD op, const struct OSRequest *io, ULONG arg);
static struct pending *newPending(struct OSRequest *io, int kind, int where, int conn);
static void joinOthers(int h, const struct OSRequest *io)
{
    char name[64];
    ULONG n = io->os_Buf[0].ob_Length;
    int i, c;
    if (n > sizeof name - 1)
        n = sizeof name - 1;
    memcpy(name, io->os_Buf[0].ob_Data, n);
    name[n] = 0;
    for (i = 0; i < cradleCount; i++) {
        struct mdns_cradle *cr = &cradles[i];
        struct pending *p;
        if (!cr->address || !mdns_offers(cr, name) || !openservice_paired(cr->fingerprint))
            continue;
        c = lanConnect(cr);
        if (c < 0 || c == handles[h].conn)
            continue;
        p = newPending(NULL, KIND_JOIN, WHERE_LAN, c);
        if (!p)
            return;
        p->handle = (UBYTE)h;
        if (!lanSubmit(p, c, 0, DIR_OPEN, io, 0))
            p->id = 0;
    }
}

/* Of the Cradles a handle is open on, the one with the fewest requests out. */
static int quietest(const struct handle *hd, UWORD *remote)
{
    int best = hd->conn, bestLoad = 0x7fff, k, i;
    *remote = hd->remote;
    for (k = -1; k < (int)hd->more; k++) {
        int c = k < 0 ? hd->conn : hd->moreConn[k], load = 0;
        for (i = 0; i < MAX_PENDING; i++)
            if (pending[i].id && pending[i].where == WHERE_LAN && pending[i].conn == c)
                load++;
        if (load < bestLoad) {
            bestLoad = load;
            best = c;
            *remote = k < 0 ? hd->remote : hd->moreRemote[k];
        }
    }
    return best;
}

static void connLost(int c)
{
    int i;
    if (conns[c].socket >= 0)
        CloseSocket(conns[c].socket);
    conns[c].socket = -1;
    if (conns[c].record)
        FreeVec(conns[c].record);
    conns[c].record = NULL;
    conns[c].sealed = 0;
    for (i = 0; i < MAX_PENDING; i++)
        if (pending[i].id && pending[i].where == WHERE_LAN && pending[i].conn == c)
            completed(&pending[i], OSERR_LOST, 0, 0);
    for (i = 0; i < MAX_HANDLES; i++) {
        struct handle *hd = &handles[i];
        int k;
        if (hd->where != WHERE_LAN)
            continue;
        for (k = 0; k < hd->more; k++)
            if (hd->moreConn[k] == c) {
                hd->moreConn[k] = hd->moreConn[hd->more - 1];
                hd->moreRemote[k] = hd->moreRemote[--hd->more];
                k--;
            }
        if (hd->conn == c) {
            if (hd->more) {              /* another Cradle carries on */
                hd->conn = hd->moreConn[--hd->more];
                hd->remote = hd->moreRemote[hd->more];
            } else
                hd->where = WHERE_NONE;
        }
    }
    browsedAt = 0;
}

/* ---- requests ------------------------------------------------------------------- */

static void completed(struct pending *p, LONG status, ULONG result, ULONG aux)
{
    struct OSRequest *io = p->io;
    int i;
    if (io && p->where == WHERE_CARD)
        for (i = 0; i < 4; i++)
            if (io->os_Buf[i].ob_Length && (io->os_Flags & (1UL << i)))
                CacheClearE(io->os_Buf[i].ob_Data, io->os_Buf[i].ob_Length, CACRF_ClearD);
    if (io && p->kind == KIND_LIST) {
        p->id = 0;
        p->io = NULL;
        if (status == OSERR_OK)
            io->os_Private[2] += result;
        io->os_Private[3]++;
        listNext(io);
        return;
    }
    if (p->kind == KIND_JOIN) {
        struct handle *hd = &handles[p->handle];
        if (status == OSERR_OK && hd->where == WHERE_LAN && hd->more < MAX_CONNS && hd->conn != p->conn) {
            hd->moreConn[hd->more] = p->conn;
            hd->moreRemote[hd->more++] = (UWORD)result;
            conns[p->conn].users++;
        }
        p->id = 0;
        return;
    }
    if (io && p->kind == KIND_OPEN) {
        if (status == OSERR_OK) {
            for (i = 0; i < MAX_HANDLES && handles[i].where; i++)
                ;
            if (i == MAX_HANDLES)
                status = OSERR_HOST;
            else {
                handles[i].where = p->where;
                handles[i].conn = p->conn;
                handles[i].remote = (UWORD)result;
                handles[i].more = 0;
                if (p->where == WHERE_LAN) {
                    conns[p->conn].users++;
                    joinOthers(i, io);
                }
                result = i + 1;
            }
        } else if (status == OSERR_NOSERVICE && p->where == WHERE_CARD && p->conn + 1 < boardCount) {
            /* Not on this board: ask the next. */
            p->conn++;
            if (cardSubmit(p, 0, DIR_OPEN, io, 0))
                return;
        } else if (status == OSERR_NOSERVICE && p->where == WHERE_CARD) {
            /* On no board: try the nursery. */
            int c = lanFor((const char *)io->os_Buf[0].ob_Data);
            p->where = WHERE_LAN;
            p->conn = (UBYTE)c;
            if (c >= 0 && lanSubmit(p, c, 0, DIR_OPEN, io, 0))
                return;
        }
    }
    p->id = 0;
    p->io = NULL;
    if (io)
        finish(io, status, result, aux);
}

static void submit(struct pending *p, struct OSRequest *io, UWORD service, UWORD op)
{
    if (p->where == WHERE_CARD) {
        if (!cardSubmit(p, service, op, io, 0)) {
            p->id = 0;
            AddTail(&waiting, &io->os_Req.io_Message.mn_Node);
        }
    } else if (!lanSubmit(p, p->conn, service, op, io, 0)) {
        int c = p->conn;
        p->io = NULL;
        p->id = 0;
        finish(io, OSERR_LOST, 0, 0);
        connLost(c);
    }
}

static void doOpen(struct OSRequest *io)
{
    struct pending *p;
    char name[64];
    ULONG n = io->os_Buf[0].ob_Length;
    int c;
    if (!io->os_Buf[0].ob_Data || !n) {
        finish(io, OSERR_BADREQUEST, 0, 0);
        return;
    }
    if (n > sizeof name - 1)
        n = sizeof name - 1;
    memcpy(name, io->os_Buf[0].ob_Data, n);
    name[n] = 0;
    io->os_Flags = 0;
    if (boardCount) {
        p = newPending(io, KIND_OPEN, WHERE_CARD, 0);
        if (!p) {
            finish(io, OSERR_HOST, 0, 0);
            return;
        }
        submit(p, io, 0, DIR_OPEN);
        return;
    }
    c = lanFor(name);
    if (c < 0) {
        finish(io, OSERR_NOSERVICE, 0, 0);
        return;
    }
    p = newPending(io, KIND_OPEN, WHERE_LAN, c);
    if (!p) {
        finish(io, OSERR_HOST, 0, 0);
        return;
    }
    submit(p, io, 0, DIR_OPEN);
}

static void doClose(struct OSRequest *io)
{
    UWORD h = io->os_Service;
    struct handle *hd;
    struct pending *p;
    if (!h || h > MAX_HANDLES || !handles[h - 1].where) {
        finish(io, OSERR_BADREQUEST, 0, 0);
        return;
    }
    hd = &handles[h - 1];
    p = newPending(NULL, KIND_INTERNAL, hd->where, hd->conn);
    if (p) {
        if (hd->where == WHERE_CARD)
            cardSubmit(p, 0, DIR_CLOSE, NULL, hd->remote);
        else
            lanSubmit(p, hd->conn, 0, DIR_CLOSE, NULL, hd->remote);
    }
    if (hd->where == WHERE_LAN && conns[hd->conn].users)
        conns[hd->conn].users--;
    while (hd->where == WHERE_LAN && hd->more) {      /* and on the other Cradles */
        int c = hd->moreConn[--hd->more];
        p = newPending(NULL, KIND_INTERNAL, WHERE_LAN, c);
        if (p)
            lanSubmit(p, c, 0, DIR_CLOSE, NULL, hd->moreRemote[hd->more]);
        if (conns[c].users)
            conns[c].users--;
    }
    hd->where = WHERE_NONE;
    finish(io, OSERR_OK, 0, 0);
}

/* LIST asks each board in turn, each writing after the last (os_Private[2]
 * holds the bytes so far, os_Private[3] the board), then the nursery. */
static void listLan(struct OSRequest *io)
{
    char *out = io->os_Buf[0].ob_Data;
    ULONG room = io->os_Buf[0].ob_Length, used = io->os_Private[2];
    int i;
    if (!boardCount) {                 /* the nursery only when nothing is local */
        lanFor("");
        for (i = 0; i < cradleCount; i++) {
            const char *s = cradles[i].services;
            if (!openservice_paired(cradles[i].fingerprint))
                continue;
            while (*s) {                  /* each name once, though several Cradles offer it */
                const char *end = strchr(s, ',');
                ULONG len = end ? (ULONG)(end - s) : strlen(s), at = 0;
                int seen = 0;
                while (at < used) {
                    ULONG have = strlen(out + at);
                    if (have == len && !strncmp(out + at, s, len))
                        seen = 1;
                    at += have + 1;
                }
                if (!seen && used + len + 1 <= room) {
                    memcpy(out + used, s, len);
                    used += len;
                    out[used++] = 0;
                }
                s += len + (end ? 1 : 0);
            }
        }
    }
    finish(io, OSERR_OK, used, 0);
}

static void listNext(struct OSRequest *io)
{
    struct pending *p;
    ULONG k = io->os_Private[3];
    if (k >= (ULONG)boardCount) {
        listLan(io);
        return;
    }
    p = newPending(io, KIND_LIST, WHERE_CARD, (int)k);
    if (!p) {
        finish(io, OSERR_HOST, 0, 0);
        return;
    }
    {
        struct OSRequest part = *io;          /* the rest of the buffer */
        part.os_Flags = 1;
        part.os_Buf[0].ob_Data = (UBYTE *)io->os_Buf[0].ob_Data + io->os_Private[2];
        part.os_Buf[0].ob_Length = io->os_Buf[0].ob_Length - io->os_Private[2];
        if (!part.os_Buf[0].ob_Length || !cardSubmit(p, 0, DIR_LIST, &part, 0)) {
            p->id = 0;
            p->io = NULL;
            listLan(io);
        }
    }
}

static void doList(struct OSRequest *io)
{
    io->os_Flags = 1;
    io->os_Private[2] = 0;
    io->os_Private[3] = 0;
    listNext(io);
}

static void doCall(struct OSRequest *io)
{
    UWORD h = io->os_Service;
    struct handle *hd;
    struct pending *p;
    if (!h || h > MAX_HANDLES || !handles[h - 1].where) {
        finish(io, h && h <= MAX_HANDLES ? OSERR_LOST : OSERR_BADREQUEST, 0, 0);
        return;
    }
    hd = &handles[h - 1];
    {
        UWORD remote = hd->remote;
        int c = hd->where == WHERE_LAN && hd->more ? quietest(hd, &remote) : hd->conn;
        p = newPending(io, KIND_USER, hd->where, c);
        if (!p) {
            finish(io, OSERR_HOST, 0, 0);
            return;
        }
        submit(p, io, remote, io->os_Op);
    }
}

static void dispatch(struct OSRequest *io)
{
    dbg("dispatch cmd", io->os_Req.io_Command, io->os_Service);
    if (io->os_Private[1]) {                       /* aborted before it started */
        finish(io, OSERR_CANCELLED, 0, 0);
        return;
    }
    switch (io->os_Req.io_Command) {
    case OSCMD_OPEN: doOpen(io); break;
    case OSCMD_CLOSE: doClose(io); break;
    case OSCMD_LIST: doList(io); break;
    case OSCMD_CALL: doCall(io); break;
    case OSCMD_WHERE: {
        UWORD h = io->os_Service;
        finish(io, OSERR_OK, h && h <= MAX_HANDLES ? handles[h - 1].where : 0, 0);
        break;
    }
    default:
        io->os_Req.io_Error = IOERR_NOCMD;
        ReplyMsg(&io->os_Req.io_Message);
    }
}

/* AbortIO marked these (os_Private[1]); tell their service. */
static void sendCancels(void)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++) {
        struct pending *p = &pending[i];
        struct pending *c;
        if (!p->id || !p->io || p->cancelSent || !p->io->os_Private[1])
            continue;
        p->cancelSent = 1;
        c = newPending(NULL, KIND_INTERNAL, p->where, p->conn);
        if (!c)
            continue;
        if (p->where == WHERE_CARD)
            cardSubmit(c, 0, OP_CANCEL, NULL, p->id);
        else
            lanSubmit(c, p->conn, 0, OP_CANCEL, NULL, p->id);
    }
}

/* Requests waiting for room on the card that AbortIO marked. */
static void cancelWaiting(void)
{
    struct Node *n, *next;
    for (n = waiting.lh_Head; (next = n->ln_Succ); n = next)
        if (((struct OSRequest *)n)->os_Private[1]) {
            Remove(n);
            finish((struct OSRequest *)n, OSERR_CANCELLED, 0, 0);
        }
}

static void retryWaiting(void)
{
    struct Node *n;
    while ((n = RemHead(&waiting)))
        dispatch((struct OSRequest *)n);
}

static void workerMain(void)
{
    struct MsgPort *port = CreateMsgPort();
    int i, running = 1;
    for (i = 0; i < MAX_CONNS; i++)
        conns[i].socket = -1;
    waiting.lh_Head = (struct Node *)&waiting.lh_Tail;
    waiting.lh_Tail = NULL;
    waiting.lh_TailPred = (struct Node *)&waiting.lh_Head;
    cardSignal = AllocSignal(-1);
    abortSignal = AllocSignal(-1);
    quitSignal = AllocSignal(-1);   /* not Ctrl-C: bsdsocket's WaitSelect takes that one for itself */
    {
        BPTR probe = Lock((CONST_STRPTR)"ENV:OpenService/Debug", ACCESS_READ);
        if (probe) {
            UnLock(probe);
            debugLog = Open((CONST_STRPTR)"T:openservice.log", MODE_NEWFILE);
        }
    }
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    dbg("socketbase", (ULONG)SocketBase, 0);
    if (port && cardSignal >= 0 && abortSignal >= 0 && quitSignal >= 0)
        cardStart();
    Forbid();
    workerPort = port;
    Signal(starter, SIGF_SINGLE);
    if (!port || cardSignal < 0 || abortSignal < 0 || quitSignal < 0)
        running = 0;
    Permit();
    while (running) {
        ULONG mask = 1UL << port->mp_SigBit | 1UL << cardSignal | 1UL << abortSignal | 1UL << quitSignal;
        ULONG got;
        struct Message *m;
        fd_set ready;
        LONG top = -1;
        FD_ZERO(&ready);
        for (i = 0; i < MAX_CONNS; i++)
            if (conns[i].socket >= 0) {
                FD_SET(conns[i].socket, &ready);
                if (conns[i].socket > top)
                    top = conns[i].socket;
            }
        if (top >= 0) {
            /* A second at most, and signals checked again after: a Ctrl-C
             * that lands while WaitSelect sets up must not be lost. */
            struct timeval second;
            second.tv_sec = 1;
            second.tv_usec = 0;
            got = mask;
            LONG r;
            dbg("waitselect top/got", top, got);
            r = WaitSelect(top + 1, &ready, NULL, NULL, &second, &got);
            dbg("waitselect r/got", r, got);
            if (r > 0)
                for (i = 0; i < MAX_CONNS; i++)
                    if (conns[i].socket >= 0 && FD_ISSET(conns[i].socket, &ready))
                        lanReceive(i);
            got = (got & mask) | (SetSignal(0, mask) & mask);
        } else
            got = Wait(mask);
        if (got & 1UL << quitSignal)
            running = 0;
        if (boardCount && (got & 1UL << cardSignal)) {
            cardDrain();
            retryWaiting();
        }
        while ((m = GetMsg(port)))
            dispatch((struct OSRequest *)m);
        if (got & 1UL << abortSignal) {
            cancelWaiting();
            sendCancels();
        }
    }
    /* Everything still out completes as lost. */
    for (i = 0; i < MAX_PENDING; i++)
        if (pending[i].id)
            completed(&pending[i], OSERR_LOST, 0, 0);
    for (i = 0; i < MAX_CONNS; i++)
        if (conns[i].socket >= 0)
            CloseSocket(conns[i].socket);
    cardStop();
    os_random_close();
    if (SocketBase)
        CloseLibrary(SocketBase);
    SocketBase = NULL;
    if (debugLog)
        Close(debugLog);
    debugLog = 0;
    Forbid();
    if (port) {
        struct Message *m;
        while ((m = GetMsg(port)))
            ((struct IORequest *)m)->io_Error = IOERR_ABORTED, ReplyMsg(m);
        DeleteMsgPort(port);
    }
    workerPort = NULL;
    worker = NULL;
    Signal(starter, SIGF_SINGLE);
    /* Forbid() holds until this process ends. */
}

static int startWorker(void)
{
    if (worker)
        return workerPort != NULL;
    starter = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    worker = CreateNewProcTags(NP_Entry, (ULONG)workerMain, NP_Name, (ULONG)DevName,
                               NP_Priority, 5, NP_StackSize, 16384, TAG_DONE);
    if (!worker)
        return 0;
    Wait(SIGF_SINGLE);
    if (!workerPort) {
        worker = NULL;
        return 0;
    }
    return 1;
}

static void stopWorker(void)
{
    if (!worker)
        return;
    starter = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    Signal(&worker->pr_Task, 1UL << quitSignal);
    Wait(SIGF_SINGLE);
}

/* ---- the device ------------------------------------------------------------------ */

static struct Device *devInit(REG(d0, struct Device *dev), REG(a0, BPTR seg), REG(a6, struct ExecBase *sysBase))
{
    SysBase = sysBase;
    segList = seg;
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)DevName;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = DEV_VERSION;
    dev->dd_Library.lib_Revision = DEV_REVISION;
    dev->dd_Library.lib_IdString = (APTR)DevIdString;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    if (!DOSBase) {
        FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize, dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
        return NULL;
    }
    return dev;
}

static void devOpen(REG(a1, struct IORequest *io), REG(d0, ULONG unit), REG(d1, ULONG flags), REG(a6, struct Device *dev))
{
    (void)unit;
    (void)flags;
    dev->dd_Library.lib_OpenCnt++;               /* no expunge while the worker starts */
    if (!startWorker()) {
        dev->dd_Library.lib_OpenCnt--;
        io->io_Error = IOERR_OPENFAIL;
        return;
    }
    io->io_Device = dev;
    io->io_Unit = NULL;
    io->io_Error = 0;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    dev->dd_Library.lib_Flags &= ~LIBF_DELEXP;
}

static BPTR devExpunge(REG(a6, struct Device *dev))
{
    BPTR seg;
    if (dev->dd_Library.lib_OpenCnt) {
        dev->dd_Library.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    stopWorker();
    Remove(&dev->dd_Library.lib_Node);
    CloseLibrary((struct Library *)DOSBase);
    seg = segList;
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize, dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
    return seg;
}

static BPTR devClose(REG(a1, struct IORequest *io), REG(a6, struct Device *dev))
{
    io->io_Device = (struct Device *)-1;
    if (--dev->dd_Library.lib_OpenCnt == 0) {
        stopWorker();
        if (dev->dd_Library.lib_Flags & LIBF_DELEXP)
            return devExpunge(dev);
    }
    return 0;
}

static ULONG devNull(void)
{
    return 0;
}

static void devBeginIO(REG(a1, struct IORequest *io), REG(a6, struct Device *dev))
{
    (void)dev;
    io->io_Flags &= ~IOF_QUICK;
    io->io_Error = 0;
    ((struct OSRequest *)io)->os_Private[1] = 0;
    if (!workerPort) {
        struct OSRequest *os = (struct OSRequest *)io;
        os->os_Status = OSERR_LOST;
        os->os_Result = os->os_Aux = 0;
        ReplyMsg(&io->io_Message);
        return;
    }
    PutMsg(workerPort, &io->io_Message);
}

static LONG devAbortIO(REG(a1, struct IORequest *io), REG(a6, struct Device *dev))
{
    (void)dev;
    /* The worker finishes it: a queued request completes as cancelled, one
     * in flight once its service has been told. */
    Forbid();
    if (io->io_Message.mn_Node.ln_Type != NT_REPLYMSG) {
        ((struct OSRequest *)io)->os_Private[1] = 1;
        if (worker)
            Signal(&worker->pr_Task, 1UL << abortSignal);
    }
    Permit();
    return 0;
}

static const APTR funcTable[] = {
    (APTR)devOpen,
    (APTR)devClose,
    (APTR)devExpunge,
    (APTR)devNull,
    (APTR)devBeginIO,
    (APTR)devAbortIO,
    (APTR)-1
};

static const ULONG initTable[] = {
    sizeof(struct Device),
    (ULONG)funcTable,
    0,
    (ULONG)devInit
};

const struct Resident os_romtag __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&os_romtag,
    (APTR)(&os_romtag + 1),
    RTF_AUTOINIT,
    DEV_VERSION,
    NT_DEVICE,
    0,
    (char *)DevName,
    (char *)DevIdString,
    (APTR)initTable
};
