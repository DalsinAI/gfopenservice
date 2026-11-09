/*
 * mitchy.library: named services on Mitchy (the services card), the other
 * ACSV boards and paired Cradles (include/libraries/mitchy.h).
 *
 * A bare library: no C library and no start-up code; start() and the
 * RomTag lead the hunk. Everything goes through gfopenservice.device,
 * which owns every ACSV board's rings and the LAN connections: each open
 * service is one opening of the device, and each MitchyRequest one
 * OSRequest of its own. The autodoc is docs/mitchy.doc, made from the
 * comments here by library/autodoc.py.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include "libraries/mitchy.h"
#include "devices/gfopenservice.h"

#define REG(r, decl) register decl __asm(#r)

#define LIB_VERSION  1
#define LIB_REVISION 0

struct ExecBase *SysBase;
struct ExpansionBase *ExpansionBase;

int start(void) { return -1; }      /* run as a program: nothing (stays first in the file) */

static const char lib_name[] = MITCHYNAME;
static const char lib_version[] __attribute__((used)) = "$VER: mitchy.library 1.0 (9.11.2026) Mitchy, the services card, AmiCradle Team";
#define lib_id (lib_version + 6)

struct MitchyBase {
    struct Library lib;
    BPTR seg;
};

/* An open service: the public part, then the device opened for it. */
struct MSvc {
    struct MitchyService pub;
    struct OSRequest io;           /* opened on the device; a template for its requests */
};

#define MREQ_MAGIC 0x4D524551UL    /* 'MREQ' */
enum { RQ_IDLE, RQ_OUT };

/* A request: the public part, then its OSRequest and, made with no port,
 * a port of its own on a signal of the task that made it. */
struct MReq {
    struct MitchyRequest pub;
    ULONG magic;
    UBYTE state;
    BYTE sig;                      /* -1: completions go to the caller's port */
    struct Task *owner;
    struct OSRequest io;
    struct MsgPort own;
};

static struct Library *lib_init(REG(d0, struct MitchyBase *base), REG(a0, BPTR seg), REG(a6, struct ExecBase *sys));
static struct Library *lib_open(REG(a6, struct MitchyBase *base));
static BPTR lib_close(REG(a6, struct MitchyBase *base));
static BPTR lib_expunge(REG(a6, struct MitchyBase *base));
static ULONG lib_null(void);

static struct MitchyService *L_MitchyOpen(REG(a0, CONST_STRPTR name), REG(d0, ULONG flags), REG(a1, LONG *error), REG(a6, struct MitchyBase *base));
static VOID L_MitchyClose(REG(a0, struct MitchyService *svc), REG(a6, struct MitchyBase *base));
static LONG L_MitchyWhere(REG(a0, struct MitchyService *svc), REG(a6, struct MitchyBase *base));
static struct MitchyRequest *L_MitchyAllocRequest(REG(a0, struct MitchyService *svc), REG(a1, struct MsgPort *port), REG(a6, struct MitchyBase *base));
static VOID L_MitchyFreeRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base));
static LONG L_MitchyDoRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base));
static LONG L_MitchySendRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base));
static BOOL L_MitchyCheckRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base));
static LONG L_MitchyWaitRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base));
static LONG L_MitchyAbortRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base));
static LONG L_MitchyCall(REG(a0, struct MitchyService *svc), REG(d0, ULONG op), REG(d1, ULONG flags), REG(d2, ULONG arg),
                         REG(a1, struct MitchyBuffer *bufs), REG(a2, ULONG *extra), REG(a3, ULONG *resultAux), REG(a6, struct MitchyBase *base));
static LONG L_MitchyList(REG(a0, STRPTR buf), REG(d0, ULONG size), REG(a6, struct MitchyBase *base));
static struct ConfigDev *L_MitchyFindBoard(REG(a0, struct ConfigDev *prev), REG(d0, ULONG klass), REG(a6, struct MitchyBase *base));
static CONST_STRPTR L_MitchyStatusText(REG(d0, LONG status), REG(a6, struct MitchyBase *base));
static struct MitchyRequest *L_MitchyRequestFromMsg(REG(a0, struct Message *msg), REG(a6, struct MitchyBase *base));

static const APTR lib_vectors[] = {
    (APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
    (APTR)L_MitchyOpen, (APTR)L_MitchyClose, (APTR)L_MitchyWhere,
    (APTR)L_MitchyAllocRequest, (APTR)L_MitchyFreeRequest,
    (APTR)L_MitchyDoRequest, (APTR)L_MitchySendRequest, (APTR)L_MitchyCheckRequest,
    (APTR)L_MitchyWaitRequest, (APTR)L_MitchyAbortRequest,
    (APTR)L_MitchyCall, (APTR)L_MitchyList, (APTR)L_MitchyFindBoard,
    (APTR)L_MitchyStatusText, (APTR)L_MitchyRequestFromMsg,
    (APTR)-1,
};
static const struct { ULONG size; const APTR *vectors; APTR data; APTR init; } lib_inittable = {
    sizeof(struct MitchyBase), lib_vectors, NULL, (APTR)lib_init,
};
const struct Resident lib_romtag __attribute__((used)) = {
    RTC_MATCHWORD, (struct Resident *)&lib_romtag, (APTR)(&lib_romtag + 1),
    RTF_AUTOINIT, LIB_VERSION, NT_LIBRARY, 0, (char *)lib_name, (char *)lib_id, (APTR)&lib_inittable,
};

/* ---- small helpers (no C library; GCC may call memcpy and memset itself) ---- */

void *memcpy(void *to, const void *from, unsigned long n)
{
    UBYTE *d = to; const UBYTE *s = from;
    while (n--) *d++ = *s++;
    return to;
}
void *memset(void *p, int c, unsigned long n)
{
    UBYTE *d = p;
    while (n--) *d++ = (UBYTE)c;
    return p;
}
static ULONG slen(CONST_STRPTR s) { ULONG n = 0; while (s[n]) n++; return n; }

/* ---- the library ------------------------------------------------------------ */

static struct Library *lib_init(REG(d0, struct MitchyBase *base), REG(a0, BPTR seg), REG(a6, struct ExecBase *sys))
{
    SysBase = sys;
    base->seg = seg;
    base->lib.lib_Node.ln_Type = NT_LIBRARY;
    base->lib.lib_Node.ln_Name = (char *)lib_name;
    base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib.lib_Version = LIB_VERSION;
    base->lib.lib_Revision = LIB_REVISION;
    base->lib.lib_IdString = (APTR)lib_id;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 37);
    if (!ExpansionBase) {
        FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
        return NULL;
    }
    return &base->lib;
}

static struct Library *lib_open(REG(a6, struct MitchyBase *base))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    return &base->lib;
}

static BPTR lib_expunge(REG(a6, struct MitchyBase *base))
{
    BPTR seg;
    if (base->lib.lib_OpenCnt) {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    Remove(&base->lib.lib_Node);
    CloseLibrary((struct Library *)ExpansionBase);
    ExpansionBase = NULL;
    seg = base->seg;
    FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seg;
}

static BPTR lib_close(REG(a6, struct MitchyBase *base))
{
    if (--base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
        return lib_expunge(base);
    return 0;
}

static ULONG lib_null(void) { return 0; }

/* ---- helpers ------------------------------------------------------------------ */

/* An ACSV board of this CLASS (0: any) after prev. */
static struct ConfigDev *findBoard(struct ConfigDev *prev, ULONG klass)
{
    struct ConfigDev *cd = prev;
    while ((cd = FindConfigDev(cd, MITCHY_MANUFACTURER, -1))) {
        volatile ULONG *regs = (volatile ULONG *)cd->cd_BoardAddr;
        if ((cd->cd_Rom.er_Type & ERTF_MEMLIST) || (cd->cd_Flags & CDF_SHUTUP) || !regs)
            continue;
        if (regs[0] != MITCHY_MAGIC || regs[1] != 1)
            continue;
        if (klass && regs[0x20 / 4] != klass)
            continue;
        return cd;
    }
    return NULL;
}

/* One command on the device, waited for, with a port made for it (so any
 * task may call). io holds the device; its other fields are set here. */
static LONG devDo(struct OSRequest *io, UWORD command, UWORD service, APTR data, ULONG length, ULONG flags)
{
    struct MsgPort *port = CreateMsgPort();
    LONG status;
    if (!port)
        return MERR_NOMEMORY;
    io->os_Req.io_Message.mn_ReplyPort = port;
    io->os_Req.io_Message.mn_Length = sizeof *io;
    io->os_Req.io_Command = command;
    io->os_Service = service;
    io->os_Op = 0;
    io->os_Flags = flags;
    io->os_Arg = 0;
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    memset(io->os_Extra, 0, sizeof io->os_Extra);
    io->os_Buf[0].ob_Data = data;
    io->os_Buf[0].ob_Length = length;
    io->os_Status = io->os_Result = io->os_Aux = 0;
    DoIO((struct IORequest *)io);
    status = io->os_Req.io_Error == IOERR_ABORTED ? MERR_CANCELLED : io->os_Req.io_Error ? MERR_BADREQUEST : io->os_Status;
    io->os_Req.io_Message.mn_ReplyPort = NULL;
    DeleteMsgPort(port);
    return status;
}

static struct MReq *mreq(struct MitchyRequest *req)
{
    struct MReq *r = (struct MReq *)req;
    return r && r->magic == MREQ_MAGIC ? r : NULL;
}

static struct MsgPort *replyPort(struct MReq *r) { return r->io.os_Req.io_Message.mn_ReplyPort; }

/* The request's completion off its reply port, if it is still there. */
static void reap(struct MReq *r)
{
    struct MsgPort *port = replyPort(r);
    struct Node *n;
    Disable();
    for (n = port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n == &r->io.os_Req.io_Message.mn_Node) {
            Remove(n);
            break;
        }
    Enable();
}

/* A completed request's answer into its public fields. */
static LONG collect(struct MReq *r)
{
    r->state = RQ_IDLE;
    r->pub.mr_Status = r->io.os_Req.io_Error == IOERR_ABORTED ? MERR_CANCELLED
                     : r->io.os_Req.io_Error ? MERR_BADREQUEST : r->io.os_Status;
    r->pub.mr_Result = r->io.os_Result;
    r->pub.mr_Aux = r->io.os_Aux;
    return r->pub.mr_Status;
}

static void prepare(struct MReq *r)
{
    struct MSvc *s = (struct MSvc *)r->pub.mr_Service;
    int i;
    r->io.os_Req.io_Command = OSCMD_CALL;
    r->io.os_Service = s->pub.ms_Handle;
    r->io.os_Op = r->pub.mr_Op;
    r->io.os_Flags = r->pub.mr_Flags;
    r->io.os_Arg = r->pub.mr_Arg;
    for (i = 0; i < 4; i++) {
        r->io.os_Buf[i].ob_Data = r->pub.mr_Buf[i].mb_Data;
        r->io.os_Buf[i].ob_Length = r->pub.mr_Buf[i].mb_Length;
        r->io.os_Extra[i] = r->pub.mr_Extra[i];
    }
    r->io.os_Status = r->io.os_Result = r->io.os_Aux = 0;
    r->pub.mr_Status = r->pub.mr_Result = r->pub.mr_Aux = 0;
}

/* Waits for a request in flight to come back, then takes it off its port. */
static LONG waitDone(struct MReq *r)
{
    struct MsgPort *port = replyPort(r);
    ULONG mask = 1UL << port->mp_SigBit;
    while (r->io.os_Req.io_Message.mn_Node.ln_Type != NT_REPLYMSG)
        Wait(mask);
    reap(r);
    if (r->sig < 0 && port->mp_MsgList.lh_Head->ln_Succ)
        SetSignal(mask, mask);       /* the caller's port has other messages: leave its signal set */
    return collect(r);
}

/****** mitchy.library/MitchyOpen ******************************************
*
*   NAME
*       MitchyOpen -- open a service by name. (V1)
*
*   SYNOPSIS
*       service = MitchyOpen(name, flags, error)
*       D0                   A0    D0     A1
*
*       struct MitchyService *MitchyOpen(CONST_STRPTR, ULONG, LONG *);
*
*   FUNCTION
*       Asks for a service by name and version, such as "echo/1" or
*       "media.decode/1": first of each ACSV board in this machine in turn
*       (Mitchy, AkikoPlus, the CPU cores board), then of the Cradles this
*       Amiga is paired with on the LAN. The handle may be used from any
*       task; each request belongs to the task that made it.
*
*   INPUTS
*       name  - the service's name, at most 63 characters.
*       flags - MOF_BOARDONLY: a board in this machine only, never the LAN.
*       error - where the reason goes when it fails (MITCHY_OK when it
*               opens), or NULL.
*
*   RESULT
*       service - the open service, or NULL:
*           MERR_NOSERVICE  no board or Cradle offers it;
*           MERR_NOTFITTED  there is no ACSV board in this machine, and no
*                           paired Cradle offers it: the card is not
*                           fitted, so do the work on the 68k;
*           MERR_NODEVICE   gfopenservice.device is not installed;
*           MERR_BADREQUEST no name, or one too long;
*           MERR_NOMEMORY.
*
*   SEE ALSO
*       MitchyClose(), MitchyWhere(), MitchyFindBoard()
*
*****************************************************************************
*/
static struct MitchyService *L_MitchyOpen(REG(a0, CONST_STRPTR name), REG(d0, ULONG flags), REG(a1, LONG *error), REG(a6, struct MitchyBase *base))
{
    struct MSvc *s;
    ULONG n;
    LONG status;
    int boards;
    (void)base;
    if (error)
        *error = MITCHY_OK;
    if (!name || !(n = slen(name)) || n > 63) {
        if (error)
            *error = MERR_BADREQUEST;
        return NULL;
    }
    boards = findBoard(NULL, 0) != NULL;
    if (!boards && (flags & MOF_BOARDONLY)) {
        if (error)
            *error = MERR_NOTFITTED;
        return NULL;
    }
    s = AllocVec(sizeof *s, MEMF_PUBLIC | MEMF_CLEAR);
    if (!s) {
        if (error)
            *error = MERR_NOMEMORY;
        return NULL;
    }
    memcpy(s->pub.ms_Name, name, n);
    s->io.os_Req.io_Message.mn_Length = sizeof s->io;
    if (OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)&s->io, 0)) {
        FreeVec(s);
        if (error)
            *error = MERR_NODEVICE;
        return NULL;
    }
    status = devDo(&s->io, OSCMD_OPEN, 0, (APTR)name, n, 0);
    if (status == MITCHY_OK) {
        s->pub.ms_Handle = (UWORD)s->io.os_Result;
        if (devDo(&s->io, OSCMD_WHERE, s->pub.ms_Handle, NULL, 0, 0) == MITCHY_OK)
            s->pub.ms_Where = (UWORD)s->io.os_Result;
        if ((flags & MOF_BOARDONLY) && s->pub.ms_Where != MITCHY_WHERE_BOARD) {
            devDo(&s->io, OSCMD_CLOSE, s->pub.ms_Handle, NULL, 0, 0);
            status = MERR_NOSERVICE;
        }
    }
    if (status != MITCHY_OK) {
        CloseDevice((struct IORequest *)&s->io);
        FreeVec(s);
        if (error)
            *error = status == MERR_NOSERVICE && !boards ? MERR_NOTFITTED : status;
        return NULL;
    }
    return &s->pub;
}

/****** mitchy.library/MitchyClose *****************************************
*
*   NAME
*       MitchyClose -- close a service. (V1)
*
*   SYNOPSIS
*       MitchyClose(service)
*                   A0
*
*       VOID MitchyClose(struct MitchyService *);
*
*   FUNCTION
*       Closes a service MitchyOpen gave. Free its requests first
*       (MitchyFreeRequest). NULL is allowed.
*
*****************************************************************************
*/
static VOID L_MitchyClose(REG(a0, struct MitchyService *svc), REG(a6, struct MitchyBase *base))
{
    struct MSvc *s = (struct MSvc *)svc;
    (void)base;
    if (!s)
        return;
    devDo(&s->io, OSCMD_CLOSE, s->pub.ms_Handle, NULL, 0, 0);
    CloseDevice((struct IORequest *)&s->io);
    FreeVec(s);
}

/****** mitchy.library/MitchyWhere *****************************************
*
*   NAME
*       MitchyWhere -- where a service runs. (V1)
*
*   SYNOPSIS
*       where = MitchyWhere(service)
*       D0                  A0
*
*       LONG MitchyWhere(struct MitchyService *);
*
*   RESULT
*       MITCHY_WHERE_BOARD - on a board in this machine (its host);
*       MITCHY_WHERE_LAN   - on a paired Cradle on the LAN;
*       MITCHY_WHERE_NONE  - nowhere any more: its Cradle was lost. Close it
*                            and open it again, or do the work on the 68k.
*
*****************************************************************************
*/
static LONG L_MitchyWhere(REG(a0, struct MitchyService *svc), REG(a6, struct MitchyBase *base))
{
    struct MSvc *s = (struct MSvc *)svc;
    (void)base;
    if (!s)
        return MITCHY_WHERE_NONE;
    if (devDo(&s->io, OSCMD_WHERE, s->pub.ms_Handle, NULL, 0, 0) == MITCHY_OK)
        s->pub.ms_Where = (UWORD)s->io.os_Result;
    return s->pub.ms_Where;
}

/****** mitchy.library/MitchyAllocRequest **********************************
*
*   NAME
*       MitchyAllocRequest -- make a request for a service. (V1)
*
*   SYNOPSIS
*       request = MitchyAllocRequest(service, port)
*       D0                           A0       A1
*
*       struct MitchyRequest *MitchyAllocRequest(struct MitchyService *,
*                                                struct MsgPort *);
*
*   FUNCTION
*       Makes a request for the service, to be filled and sent as often as
*       wanted, one call at a time. Make it in the task that will wait for
*       it.
*
*       With port NULL, the request gets a signal of the calling task of
*       its own (mr_SigMask): it is set when a request sent with
*       MitchySendRequest completes. Wait() for it, then MitchyCheckRequest
*       or MitchyWaitRequest.
*
*       With a port, each completion is a message on that port instead
*       (many requests may share one). GetMsg() it, find its request with
*       MitchyRequestFromMsg, then call MitchyCheckRequest to take the
*       answer into its fields.
*
*   RESULT
*       request - the request, or NULL when there is no memory or (port
*           NULL) no free signal.
*
*   SEE ALSO
*       MitchyFreeRequest(), MitchyDoRequest(), MitchySendRequest()
*
*****************************************************************************
*/
static struct MitchyRequest *L_MitchyAllocRequest(REG(a0, struct MitchyService *svc), REG(a1, struct MsgPort *port), REG(a6, struct MitchyBase *base))
{
    struct MSvc *s = (struct MSvc *)svc;
    struct MReq *r;
    (void)base;
    if (!s || !(r = AllocVec(sizeof *r, MEMF_PUBLIC | MEMF_CLEAR)))
        return NULL;
    r->magic = MREQ_MAGIC;
    r->owner = FindTask(NULL);
    r->sig = -1;
    if (!port) {
        if ((r->sig = AllocSignal(-1)) < 0) {
            FreeVec(r);
            return NULL;
        }
        r->own.mp_Node.ln_Type = NT_MSGPORT;
        r->own.mp_Flags = PA_SIGNAL;
        r->own.mp_SigBit = (UBYTE)r->sig;
        r->own.mp_SigTask = r->owner;
        r->own.mp_MsgList.lh_Head = (struct Node *)&r->own.mp_MsgList.lh_Tail;   /* NewList (amiga.lib's) */
        r->own.mp_MsgList.lh_Tail = NULL;
        r->own.mp_MsgList.lh_TailPred = (struct Node *)&r->own.mp_MsgList.lh_Head;
        r->own.mp_MsgList.lh_Type = NT_MESSAGE;
        port = &r->own;
        r->pub.mr_SigMask = 1UL << r->sig;
    } else
        r->pub.mr_ReplyPort = port;
    r->pub.mr_Service = svc;
    r->io = s->io;                                    /* the device and its unit */
    r->io.os_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    r->io.os_Req.io_Message.mn_Node.ln_Name = (char *)r;   /* MitchyRequestFromMsg */
    r->io.os_Req.io_Message.mn_ReplyPort = port;
    r->io.os_Req.io_Message.mn_Length = sizeof r->io;
    r->state = RQ_IDLE;
    return &r->pub;
}

/****** mitchy.library/MitchyFreeRequest ***********************************
*
*   NAME
*       MitchyFreeRequest -- free a request. (V1)
*
*   SYNOPSIS
*       MitchyFreeRequest(request)
*                         A0
*
*       VOID MitchyFreeRequest(struct MitchyRequest *);
*
*   FUNCTION
*       Frees a request, from the task that made it. One still in flight
*       is cancelled and waited for first. NULL is allowed.
*
*****************************************************************************
*/
static VOID L_MitchyFreeRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base))
{
    struct MReq *r = mreq(req);
    (void)base;
    if (!r)
        return;
    if (r->state == RQ_OUT) {
        if (r->io.os_Req.io_Message.mn_Node.ln_Type != NT_REPLYMSG)
            AbortIO((struct IORequest *)&r->io);
        waitDone(r);
    }
    if (r->sig >= 0)
        FreeSignal(r->sig);
    r->magic = 0;
    FreeVec(r);
}

/****** mitchy.library/MitchyDoRequest *************************************
*
*   NAME
*       MitchyDoRequest -- send a request and wait for its answer. (V1)
*
*   SYNOPSIS
*       status = MitchyDoRequest(request)
*       D0                       A0
*
*       LONG MitchyDoRequest(struct MitchyRequest *);
*
*   FUNCTION
*       Sends the request as its fields say (mr_Op, mr_Flags, mr_Arg,
*       mr_Buf, mr_Extra) and waits for it. The other tasks and the Amiga
*       run on meanwhile.
*
*   RESULT
*       status - mr_Status: MITCHY_OK, a MERR_* or the service's own;
*           mr_Result and mr_Aux are the service's answer. MERR_BUSY: it
*           was in flight already.
*
*****************************************************************************
*/
static LONG L_MitchyDoRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base))
{
    struct MReq *r = mreq(req);
    (void)base;
    if (!r)
        return MERR_BADREQUEST;
    if (r->state == RQ_OUT)
        return MERR_BUSY;
    prepare(r);
    r->state = RQ_OUT;
    DoIO((struct IORequest *)&r->io);    /* WaitIO takes it off the port */
    return collect(r);
}

/****** mitchy.library/MitchySendRequest ***********************************
*
*   NAME
*       MitchySendRequest -- send a request and go on. (V1)
*
*   SYNOPSIS
*       status = MitchySendRequest(request)
*       D0                         A0
*
*       LONG MitchySendRequest(struct MitchyRequest *);
*
*   FUNCTION
*       Sends the request and returns at once. Its buffers belong to the
*       service until it completes: then its signal (mr_SigMask) is set or
*       its message arrives on its port. Many requests may be in flight at
*       once; each completes on its own.
*
*   RESULT
*       status - MITCHY_OK when sent; MERR_BUSY when it is in flight
*           already.
*
*   SEE ALSO
*       MitchyCheckRequest(), MitchyWaitRequest(), MitchyAbortRequest()
*
*****************************************************************************
*/
static LONG L_MitchySendRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base))
{
    struct MReq *r = mreq(req);
    (void)base;
    if (!r)
        return MERR_BADREQUEST;
    if (r->state == RQ_OUT)
        return MERR_BUSY;
    prepare(r);
    if (r->sig >= 0)
        SetSignal(0, 1UL << r->sig);
    r->state = RQ_OUT;
    SendIO((struct IORequest *)&r->io);
    return MITCHY_OK;
}

/****** mitchy.library/MitchyCheckRequest **********************************
*
*   NAME
*       MitchyCheckRequest -- has a request completed? (V1)
*
*   SYNOPSIS
*       done = MitchyCheckRequest(request)
*       D0                        A0
*
*       BOOL MitchyCheckRequest(struct MitchyRequest *);
*
*   FUNCTION
*       TRUE when the request is not in flight: it has completed, and its
*       answer is in mr_Status, mr_Result and mr_Aux (and its message, if
*       it has a port and was not taken with GetMsg yet, is taken off).
*       FALSE while the service still has it. Never waits.
*
*****************************************************************************
*/
static BOOL L_MitchyCheckRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base))
{
    struct MReq *r = mreq(req);
    (void)base;
    if (!r || r->state != RQ_OUT)
        return TRUE;
    if (r->io.os_Req.io_Message.mn_Node.ln_Type != NT_REPLYMSG)
        return FALSE;
    reap(r);
    collect(r);
    return TRUE;
}

/****** mitchy.library/MitchyWaitRequest ***********************************
*
*   NAME
*       MitchyWaitRequest -- wait for a request to complete. (V1)
*
*   SYNOPSIS
*       status = MitchyWaitRequest(request)
*       D0                         A0
*
*       LONG MitchyWaitRequest(struct MitchyRequest *);
*
*   FUNCTION
*       Waits until the request completes, and returns mr_Status. A
*       request not in flight returns its last status at once. Only the
*       task that made the request may wait for it.
*
*****************************************************************************
*/
static LONG L_MitchyWaitRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base))
{
    struct MReq *r = mreq(req);
    (void)base;
    if (!r)
        return MERR_BADREQUEST;
    if (r->state != RQ_OUT)
        return r->pub.mr_Status;
    return waitDone(r);
}

/****** mitchy.library/MitchyAbortRequest **********************************
*
*   NAME
*       MitchyAbortRequest -- cancel a request. (V1)
*
*   SYNOPSIS
*       status = MitchyAbortRequest(request)
*       D0                          A0
*
*       LONG MitchyAbortRequest(struct MitchyRequest *);
*
*   FUNCTION
*       Cancels a request in flight (the card's CANCEL, sent on to the
*       service) and waits for it, which is at once: the card answers a
*       CANCEL itself. A buffer the request marks written is not written
*       after this returns MERR_CANCELLED. A request that had completed
*       keeps its answer.
*
*   RESULT
*       status - MERR_CANCELLED, or the answer when it had completed.
*
*****************************************************************************
*/
static LONG L_MitchyAbortRequest(REG(a0, struct MitchyRequest *req), REG(a6, struct MitchyBase *base))
{
    struct MReq *r = mreq(req);
    (void)base;
    if (!r)
        return MERR_BADREQUEST;
    if (r->state != RQ_OUT)
        return r->pub.mr_Status;
    if (r->io.os_Req.io_Message.mn_Node.ln_Type != NT_REPLYMSG)
        AbortIO((struct IORequest *)&r->io);
    return waitDone(r);
}

/****** mitchy.library/MitchyCall ******************************************
*
*   NAME
*       MitchyCall -- one call to a service, waited for. (V1)
*
*   SYNOPSIS
*       status = MitchyCall(service, op, flags, arg, buffers, extra,
*       D0                  A0       D0  D1     D2   A1       A2
*                           resultAux)
*                           A3
*
*       LONG MitchyCall(struct MitchyService *, ULONG, ULONG, ULONG,
*                       struct MitchyBuffer *, ULONG *, ULONG *);
*
*   FUNCTION
*       MitchyAllocRequest, MitchyDoRequest and MitchyFreeRequest in one,
*       for a single call from any task.
*
*   INPUTS
*       buffers   - four MitchyBuffers, or NULL for none.
*       extra     - four ULONGs (op-specific), or NULL for zeros.
*       resultAux - two ULONGs that get the result and aux, or NULL.
*
*   RESULT
*       status - as MitchyDoRequest's.
*
*****************************************************************************
*/
static LONG L_MitchyCall(REG(a0, struct MitchyService *svc), REG(d0, ULONG op), REG(d1, ULONG flags), REG(d2, ULONG arg),
                         REG(a1, struct MitchyBuffer *bufs), REG(a2, ULONG *extra), REG(a3, ULONG *resultAux), REG(a6, struct MitchyBase *base))
{
    struct MSvc *s = (struct MSvc *)svc;
    struct OSRequest io;
    struct MsgPort *port;
    LONG status;
    int i;
    (void)base;
    if (!s)
        return MERR_BADREQUEST;
    if (!(port = CreateMsgPort()))
        return MERR_NOMEMORY;
    io = s->io;
    io.os_Req.io_Message.mn_ReplyPort = port;
    io.os_Req.io_Message.mn_Length = sizeof io;
    io.os_Req.io_Command = OSCMD_CALL;
    io.os_Service = s->pub.ms_Handle;
    io.os_Op = (UWORD)op;
    io.os_Flags = flags;
    io.os_Arg = arg;
    for (i = 0; i < 4; i++) {
        io.os_Buf[i].ob_Data = bufs ? bufs[i].mb_Data : NULL;
        io.os_Buf[i].ob_Length = bufs ? bufs[i].mb_Length : 0;
        io.os_Extra[i] = extra ? extra[i] : 0;
    }
    io.os_Status = io.os_Result = io.os_Aux = 0;
    DoIO((struct IORequest *)&io);
    status = io.os_Req.io_Error == IOERR_ABORTED ? MERR_CANCELLED : io.os_Req.io_Error ? MERR_BADREQUEST : io.os_Status;
    if (resultAux) {
        resultAux[0] = io.os_Result;
        resultAux[1] = io.os_Aux;
    }
    DeleteMsgPort(port);
    return status;
}

/****** mitchy.library/MitchyList ******************************************
*
*   NAME
*       MitchyList -- the services on offer. (V1)
*
*   SYNOPSIS
*       length = MitchyList(buffer, size)
*       D0                  A0      D0
*
*       LONG MitchyList(STRPTR, ULONG);
*
*   FUNCTION
*       Writes the names of the services every ACSV board in this machine
*       offers, NUL-separated, one board after another; with no board,
*       those of the paired Cradles (each name once).
*
*   RESULT
*       length - the bytes written, or a MERR_* (MERR_NODEVICE when
*           gfopenservice.device is not installed).
*
*****************************************************************************
*/
static LONG L_MitchyList(REG(a0, STRPTR buf), REG(d0, ULONG size), REG(a6, struct MitchyBase *base))
{
    struct OSRequest io;
    LONG status;
    (void)base;
    if (!buf || !size)
        return MERR_BADREQUEST;
    memset(&io, 0, sizeof io);
    io.os_Req.io_Message.mn_Length = sizeof io;
    if (OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)&io, 0))
        return MERR_NODEVICE;
    status = devDo(&io, OSCMD_LIST, 0, buf, size, 1);
    CloseDevice((struct IORequest *)&io);
    return status ? status : (LONG)io.os_Result;
}

/****** mitchy.library/MitchyFindBoard *************************************
*
*   NAME
*       MitchyFindBoard -- find the ACSV boards in this machine. (V1)
*
*   SYNOPSIS
*       board = MitchyFindBoard(previous, boardClass)
*       D0                      A0        D0
*
*       struct ConfigDev *MitchyFindBoard(struct ConfigDev *, ULONG);
*
*   FUNCTION
*       The next AmiCradle Team board after previous (NULL: the first)
*       that carries the ACSV register block, of this CLASS:
*       MITCHY_CLASS_SERVICES (Mitchy), MITCHY_CLASS_CORES,
*       MITCHY_CLASS_MEDIA (AkikoPlus), or MITCHY_CLASS_ANY. Use it to say
*       whether the card is fitted, or to reach a board's own registers
*       past the block (AkikoPlus's bank windows). Never write the block's
*       RINGS, ORDER, DOORBELL or INTENA: gfopenservice.device owns them.
*
*   RESULT
*       board - its ConfigDev (cd_BoardAddr, cd_BoardSize,
*           cd_Rom.er_Product), or NULL when there is no other.
*
*****************************************************************************
*/
static struct ConfigDev *L_MitchyFindBoard(REG(a0, struct ConfigDev *prev), REG(d0, ULONG klass), REG(a6, struct MitchyBase *base))
{
    (void)base;
    return findBoard(prev, klass);
}

/****** mitchy.library/MitchyStatusText ************************************
*
*   NAME
*       MitchyStatusText -- a status in words. (V1)
*
*   SYNOPSIS
*       text = MitchyStatusText(status)
*       D0                      D0
*
*       CONST_STRPTR MitchyStatusText(LONG);
*
*   RESULT
*       text - a short English text, never NULL.
*
*****************************************************************************
*/
static CONST_STRPTR L_MitchyStatusText(REG(d0, LONG status), REG(a6, struct MitchyBase *base))
{
    (void)base;
    switch (status) {
    case MITCHY_OK: return (CONST_STRPTR)"ok";
    case MERR_NOSERVICE: return (CONST_STRPTR)"no such service";
    case MERR_BADREQUEST: return (CONST_STRPTR)"bad request";
    case MERR_CANCELLED: return (CONST_STRPTR)"cancelled";
    case MERR_TOOSMALL: return (CONST_STRPTR)"buffer too small";
    case MERR_HOST: return (CONST_STRPTR)"host error or busy";
    case MERR_LOST: return (CONST_STRPTR)"board or Cradle lost";
    case MERR_NODEVICE: return (CONST_STRPTR)"gfopenservice.device not installed";
    case MERR_NOMEMORY: return (CONST_STRPTR)"no memory";
    case MERR_BUSY: return (CONST_STRPTR)"request already in flight";
    case MERR_NOTFITTED: return (CONST_STRPTR)"no services card fitted, and no paired Cradle offers it";
    case MERR_NOSIGNAL: return (CONST_STRPTR)"no free signal";
    }
    return (CONST_STRPTR)(status < 0 ? "the service's own error" : "unknown status");
}

/****** mitchy.library/MitchyRequestFromMsg ********************************
*
*   NAME
*       MitchyRequestFromMsg -- the request a completion message is for. (V1)
*
*   SYNOPSIS
*       request = MitchyRequestFromMsg(message)
*       D0                             A0
*
*       struct MitchyRequest *MitchyRequestFromMsg(struct Message *);
*
*   FUNCTION
*       For requests made with a port: a message GetMsg() took from it is
*       the completion of the request returned (then MitchyCheckRequest
*       it); NULL when the message is not a Mitchy request's.
*
*****************************************************************************
*/
static struct MitchyRequest *L_MitchyRequestFromMsg(REG(a0, struct Message *msg), REG(a6, struct MitchyBase *base))
{
    struct MReq *r;
    (void)base;
    if (!msg)
        return NULL;
    r = (struct MReq *)msg->mn_Node.ln_Name;
    if (!r || ((ULONG)r & 1) || r->magic != MREQ_MAGIC || (struct Message *)&r->io != msg)
        return NULL;
    return &r->pub;
}
