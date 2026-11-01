/*
 * webfetch: openweb.fetch through gfopenservice.device, or through the
 * native transport (src/wfnet_amiga.c) when no card or Cradle offers it
 * (include/openweb/webfetch.h). No C library beyond <string.h>, so any
 * program can link it.
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <proto/exec.h>

#include <proto/dos.h>

#include "openweb/webfetch.h"
#include "wfnet_amiga.h"

#define NATIVE_WINDOW (64UL << 10)

static LONG run(struct wf_service *s, UWORD command)
{
    s->io->os_Req.io_Command = command;
    DoIO((struct IORequest *)s->io);
    if (s->io->os_Req.io_Error)
        return s->io->os_Req.io_Error == IOERR_ABORTED ? OSERR_CANCELLED : OSERR_LOST;
    return s->io->os_Status;
}

/* Fills a request for one call: up to one buffer read (in) and one written (out, buffer 0). */
static void fill(struct wf_service *s, struct OSRequest *io, UWORD op, ULONG arg, const void *in0, ULONG in0_length,
                 const void *in1, ULONG in1_length, void *out0, ULONG out0_length, const ULONG extra[4])
{
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Service = s->handle;
    io->os_Op = op;
    io->os_Arg = arg;
    io->os_Flags = 0;
    if (out0) {
        io->os_Buf[0].ob_Data = out0;
        io->os_Buf[0].ob_Length = out0_length;
        io->os_Flags = 1;
    } else if (in0) {
        io->os_Buf[0].ob_Data = (APTR)in0;
        io->os_Buf[0].ob_Length = in0_length;
    }
    if (in1 && in1_length) {
        io->os_Buf[1].ob_Data = (APTR)in1;
        io->os_Buf[1].ob_Length = in1_length;
    }
    io->os_Extra[0] = extra ? extra[0] : 0;
    io->os_Extra[1] = extra ? extra[1] : 0;
    io->os_Extra[2] = extra ? extra[2] : 0;
    io->os_Extra[3] = extra ? extra[3] : 0;
}

/* The native transport's call, with the same buffers. */
static LONG native(struct wf_service *s, UWORD op, ULONG arg, const void *in0, ULONG in0_length,
                   const void *in1, ULONG in1_length, void *out0, ULONG out0_length, const ULONG extra[4])
{
    struct wfn_buf b[4];
    memset(b, 0, sizeof b);
    if (out0) {
        b[0].out = out0;
        b[0].length = out0_length;
    } else if (in0) {
        b[0].in = in0;
        b[0].length = in0_length;
    }
    if (in1 && in1_length) {
        b[1].in = in1;
        b[1].length = in1_length;
    }
    return wfna_call(s->native, op, arg, extra, b, &s->result, &s->aux);
}

static LONG call(struct wf_service *s, UWORD op, ULONG arg, const void *in0, ULONG in0_length,
                 const void *in1, ULONG in1_length, void *out0, ULONG out0_length, const ULONG extra[4])
{
    LONG status;
    if (!s->open)
        return OSERR_NOSERVICE;
    if (s->native)
        return native(s, op, arg, in0, in0_length, in1, in1_length, out0, out0_length, extra);
    fill(s, s->io, op, arg, in0, in0_length, in1, in1_length, out0, out0_length, extra);
    status = run(s, OSCMD_CALL);
    s->result = s->io->os_Result;
    s->aux = s->io->os_Aux;
    return status;
}

static LONG open_service(struct wf_service *s);

/* ENV:GFOpenWeb/Fetch "native" asks for the native transport. */
static BOOL native_wanted(void)
{
    char v[16];
    return GetVar((CONST_STRPTR)"GFOpenWeb/Fetch", (STRPTR)v, sizeof v, 0) == 6 && (v[0] | 32) == 'n';
}

static LONG open_native(struct wf_service *s)
{
    memset(s, 0, sizeof *s);
    if (!(s->native = wfna_new(NATIVE_WINDOW)))
        return OSERR_NOSERVICE;
    if (!(s->port = CreateMsgPort())) {
        wfna_free(s->native);
        s->native = NULL;
        return OSERR_HOST;
    }
    s->open = 1;
    s->where = WF_WHERE_NATIVE;
    return OSERR_OK;
}

LONG wf_service_open(struct wf_service *s)
{
    return wf_service_open_mode(s, WF_MODE_AUTO);
}

LONG wf_service_open_mode(struct wf_service *s, ULONG mode)
{
    LONG status;
    if (mode == WF_MODE_NATIVE || (mode == WF_MODE_AUTO && native_wanted()))
        return open_native(s);
    if ((status = open_service(s)) == OSERR_OK || mode == WF_MODE_SERVICE)
        return status;
    return open_native(s);
}

static LONG open_service(struct wf_service *s)
{
    LONG status;
    memset(s, 0, sizeof *s);
    if (!(s->port = CreateMsgPort()))
        return OSERR_HOST;
    if (!(s->io = (struct OSRequest *)CreateIORequest(s->port, sizeof *s->io))) {
        wf_service_close(s);
        return OSERR_HOST;
    }
    if (OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)s->io, 0)) {
        DeleteIORequest((struct IORequest *)s->io);
        s->io = NULL;
        wf_service_close(s);
        return OSERR_NOSERVICE;
    }
    memset(s->io->os_Buf, 0, sizeof s->io->os_Buf);
    s->io->os_Buf[0].ob_Data = (APTR)WF_SERVICE_NAME;
    s->io->os_Buf[0].ob_Length = sizeof WF_SERVICE_NAME - 1;
    if ((status = run(s, OSCMD_OPEN)) != OSERR_OK) {
        wf_service_close(s);
        return status;
    }
    s->handle = (UWORD)s->io->os_Result;
    s->open = 1;
    s->io->os_Service = s->handle;
    s->where = run(s, OSCMD_WHERE) == OSERR_OK ? (UBYTE)s->io->os_Result : 0;
    return OSERR_OK;
}

void wf_service_close(struct wf_service *s)
{
    if (s->native)
        wfna_free(s->native);
    if (s->io) {
        if (s->open) {
            s->io->os_Service = s->handle;
            run(s, OSCMD_CLOSE);
        }
        if (s->io->os_Req.io_Device)
            CloseDevice((struct IORequest *)s->io);
        DeleteIORequest((struct IORequest *)s->io);
    }
    if (s->port)
        DeleteMsgPort(s->port);
    memset(s, 0, sizeof *s);
}

LONG wf_open(struct wf_service *s, const char *request, ULONG length, ULONG flags, ULONG body_length,
             ULONG timeout, const void *body, ULONG part, ULONG *fetch)
{
    ULONG extra[4];
    LONG status;
    extra[0] = flags;
    extra[1] = (flags & WF_BODY) ? body_length : 0;
    extra[2] = timeout;
    extra[3] = 0;
    *fetch = 0;
    status = call(s, WF_OP_OPEN, 0, request, length ? length : strlen(request), (flags & WF_BODY) ? body : NULL,
                  (flags & WF_BODY) ? part : 0, NULL, 0, extra);
    if (status == OSERR_OK)
        *fetch = s->result;
    return status;
}

LONG wf_send(struct wf_service *s, ULONG fetch, const void *data, ULONG length, BOOL last, ULONG wait, ULONG *taken)
{
    ULONG extra[4];
    LONG status;
    extra[0] = last ? 1 : 0;
    extra[1] = wait;
    extra[2] = extra[3] = 0;
    status = call(s, WF_OP_SEND, fetch, length ? data : NULL, length, NULL, 0, NULL, 0, extra);
    *taken = status == OSERR_OK ? s->result : 0;
    return status;
}

static ULONG get32(const UBYTE *p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

LONG wf_head(struct wf_service *s, ULONG fetch, UBYTE *buf, ULONG room, ULONG wait, struct wf_head *h)
{
    ULONG extra[4];
    LONG status;
    extra[0] = wait;
    extra[1] = extra[2] = extra[3] = 0;
    memset(h, 0, sizeof *h);
    status = call(s, WF_OP_HEAD, fetch, NULL, 0, NULL, 0, buf, room, extra);
    if (status == OSERR_TOOSMALL)
        h->need = s->result;
    if (status != OSERR_OK)
        return status;
    if (!s->result && !s->aux)
        return WF_NOTYET;
    return wf_head_parse(buf, room, h);
}

LONG wf_head_parse(UBYTE *buf, ULONG room, struct wf_head *h)
{
    memset(h, 0, sizeof *h);
    if (room < 32 || get32(buf) != 0x4f574631UL || 32 + get32(buf + 20) + get32(buf + 24) > room)
        return OSERR_BADREQUEST;
    h->error = get32(buf + 4);
    h->status = get32(buf + 8);
    h->flags = get32(buf + 12);
    h->length = get32(buf + 16);
    h->text_length = get32(buf + 20);
    h->info_length = get32(buf + 24);
    h->version = get32(buf + 28);
    h->text = (char *)buf + 32;
    h->info = h->text + h->text_length;
    return OSERR_OK;
}

LONG wf_read(struct wf_service *s, ULONG fetch, void *buf, ULONG room, ULONG wait, ULONG *got, ULONG *end)
{
    ULONG extra[4];
    LONG status;
    extra[0] = wait;
    extra[1] = extra[2] = extra[3] = 0;
    status = call(s, WF_OP_READ, fetch, NULL, 0, NULL, 0, buf, room, extra);
    *got = status == OSERR_OK ? s->result : 0;
    *end = status == OSERR_OK ? s->aux : 1;
    return status;
}

/* ---- WebSocket ---- */

LONG wf_ws_open(struct wf_service *s, const char *request, ULONG length, ULONG flags, ULONG timeout, ULONG *fetch)
{
    ULONG extra[4];
    LONG status;
    extra[0] = flags & ~WF_BODY;
    extra[1] = 0;
    extra[2] = timeout;
    extra[3] = 0;
    *fetch = 0;
    status = call(s, WF_OP_WS_OPEN, 0, request, length ? length : strlen(request), NULL, 0, NULL, 0, extra);
    if (status == OSERR_OK)
        *fetch = s->result;
    return status;
}

LONG wf_ws_send(struct wf_service *s, ULONG fetch, const void *data, ULONG length, ULONG op, ULONG wait, ULONG *taken)
{
    ULONG extra[4];
    LONG status;
    extra[0] = op;
    extra[1] = wait;
    extra[2] = extra[3] = 0;
    status = call(s, WF_OP_WS_SEND, fetch, length ? data : NULL, length, NULL, 0, NULL, 0, extra);
    *taken = status == OSERR_OK ? s->result : 0;
    return status;
}

LONG wf_ws_recv(struct wf_service *s, ULONG fetch, void *buf, ULONG room, ULONG wait, ULONG *got, ULONG *flags)
{
    ULONG extra[4];
    LONG status;
    extra[0] = wait;
    extra[1] = extra[2] = extra[3] = 0;
    status = call(s, WF_OP_WS_RECV, fetch, NULL, 0, NULL, 0, buf, room, extra);
    *got = status == OSERR_OK ? s->result : 0;
    *flags = status == OSERR_OK ? s->aux : WF_WS_END;
    return status;
}

/* ---- many fetches from one task ---- */

/* Whether an answer to HEAD, READ or WS_RECV says something (not "not yet"). */
static BOOL answered(struct wf_async *a)
{
    return a->status != OSERR_OK || a->result || a->aux;
}

LONG wf_async_start(struct wf_service *s, struct wf_async *a, UWORD op, ULONG fetch, void *buf, ULONG room, ULONG wait)
{
    ULONG extra[4];
    memset(a, 0, sizeof *a);
    if (!s->open)
        return OSERR_NOSERVICE;
    if (op != WF_OP_HEAD && op != WF_OP_READ && op != WF_OP_WS_RECV)
        return OSERR_BADREQUEST;
    a->s = s, a->op = op, a->fetch = fetch, a->buf = buf, a->room = room, a->wait = wait;
    extra[0] = wait;
    extra[1] = extra[2] = extra[3] = 0;
    if (s->native) {
        /* Asked at once without waiting; wf_wait moves it on, wf_async_check asks again. */
        extra[0] = 0;
        a->status = native(s, op, fetch, NULL, 0, NULL, 0, buf, room, extra);
        a->result = s->result, a->aux = s->aux;
        a->done = answered(a);
        a->busy = !a->done;
        return OSERR_OK;
    }
    if (!(a->io = (struct OSRequest *)CreateIORequest(s->port, sizeof *a->io)))
        return OSERR_HOST;
    a->io->os_Req.io_Device = s->io->os_Req.io_Device;
    a->io->os_Req.io_Unit = s->io->os_Req.io_Unit;
    a->io->os_Req.io_Command = OSCMD_CALL;
    fill(s, a->io, op, fetch, NULL, 0, NULL, 0, buf, room, extra);
    SendIO((struct IORequest *)a->io);
    a->busy = 1;
    return OSERR_OK;
}

BOOL wf_async_check(struct wf_async *a)
{
    if (a->done || !a->busy)
        return a->done;
    if (a->s->native) {
        static const ULONG none[4] = { 0, 0, 0, 0 };
        a->status = native(a->s, a->op, a->fetch, NULL, 0, NULL, 0, a->buf, a->room, none);
        a->result = a->s->result, a->aux = a->s->aux;
        if (answered(a))
            a->done = 1, a->busy = 0;
        return a->done;
    }
    if (!CheckIO((struct IORequest *)a->io))
        return FALSE;
    WaitIO((struct IORequest *)a->io);
    a->status = a->io->os_Req.io_Error ? (a->io->os_Req.io_Error == IOERR_ABORTED ? OSERR_CANCELLED : OSERR_LOST)
                                       : a->io->os_Status;
    a->result = a->io->os_Result, a->aux = a->io->os_Aux;
    DeleteIORequest((struct IORequest *)a->io);
    a->io = NULL;
    a->busy = 0, a->done = 1;
    return TRUE;
}

void wf_async_abort(struct wf_async *a)
{
    if (a->io) {
        if (!CheckIO((struct IORequest *)a->io))
            AbortIO((struct IORequest *)a->io);
        WaitIO((struct IORequest *)a->io);
        DeleteIORequest((struct IORequest *)a->io);
        a->io = NULL;
    }
    a->busy = 0;
}

ULONG wf_wait(struct wf_service *s, ULONG ms, ULONG sigs)
{
    ULONG got;
    if (!s->open)
        return 0;
    if (s->native)
        return wfna_wait(s->native, ms, sigs);
    /* An answer already on the port (its signal may have gone to a DoIO meanwhile) wakes at once. */
    Forbid();
    got = s->port->mp_MsgList.lh_Head->ln_Succ != NULL;
    Permit();
    if (got)
        return SetSignal(0, sigs) & sigs;
    return Wait(sigs | 1UL << s->port->mp_SigBit) & sigs;
}

LONG wf_cancel(struct wf_service *s, ULONG fetch)
{
    return call(s, WF_OP_CANCEL, fetch, NULL, 0, NULL, 0, NULL, 0, NULL);
}

LONG wf_close(struct wf_service *s, ULONG fetch)
{
    return call(s, WF_OP_CLOSE, fetch, NULL, 0, NULL, 0, NULL, 0, NULL);
}

static ULONG put(char *out, ULONG at, ULONG room, const char *s)
{
    ULONG n = strlen(s);
    if (at + n >= room)
        return room;                            /* does not fit */
    memcpy(out + at, s, n);
    return at + n;
}

ULONG wf_request(char *out, ULONG room, const char *method, const char *url, const char *const *headers, LONG count)
{
    ULONG at = 0;
    LONG i;
    at = put(out, at, room, method);
    at = put(out, at, room, " ");
    at = put(out, at, room, url);
    at = put(out, at, room, "\n");
    for (i = 0; i < count && at < room; i++) {
        at = put(out, at, room, headers[i]);
        at = put(out, at, room, "\n");
    }
    if (at >= room)
        return 0;
    out[at] = 0;
    return at;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

BOOL wf_header(const struct wf_head *h, const char *name, char *out, ULONG room)
{
    const char *p = h->text, *end = h->text + h->text_length;
    ULONG n = strlen(name);
    if (!p || !room)
        return FALSE;
    while (p < end) {
        const char *nl = p;
        ULONG i;
        while (nl < end && *nl != '\n')
            nl++;
        for (i = 0; i < n && p + i < nl && lower(p[i]) == lower(name[i]); i++)
            ;
        if (i == n && p + n < nl && p[n] == ':') {
            const char *v = p + n + 1;
            ULONG k = 0;
            while (v < nl && (*v == ' ' || *v == '\t'))
                v++;
            while (v < nl && k + 1 < room)
                out[k++] = *v++;
            out[k] = 0;
            return TRUE;
        }
        p = nl + 1;
    }
    return FALSE;
}

const char *wf_error_name(ULONG error)
{
    static const char *const names[] = { "none", "DNS", "connect", "TLS", "timeout", "protocol",
                                         "cancelled", "closed early", "decode", "no room for it" };
    return error < sizeof names / sizeof names[0] ? names[error] : "unknown";
}
