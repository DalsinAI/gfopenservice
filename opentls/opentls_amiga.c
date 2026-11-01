/*
 * opentls on the Amiga: the transport is gfopenservice.device's opentls.key/1
 * (opentls_amiga.h). Each request is one DoIO from the calling task, so
 * curl's threads can all use it at once.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <proto/exec.h>

#include "devices/gfopenservice.h"
#include "opentls.h"
#include "opentls_amiga.h"

static struct MsgPort *openPort;
static struct OSRequest *opened;      /* the request that opened the device */
static UWORD handle;
static ULONG where;

static int amigaTransport(uint16_t op, uint32_t arg, const uint32_t extra[4], struct otk_buffer buf[4],
                          uint32_t *result, uint32_t *aux)
{
    struct MsgPort *port = CreateMsgPort();
    struct OSRequest *io = port ? (struct OSRequest *)CreateIORequest(port, sizeof *io) : NULL;
    LONG status = OSERR_LOST;
    int i;
    if (io) {
        io->os_Req.io_Device = opened->os_Req.io_Device;
        io->os_Req.io_Unit = opened->os_Req.io_Unit;
        io->os_Req.io_Command = OSCMD_CALL;
        io->os_Service = handle;
        io->os_Op = op;
        io->os_Arg = arg;
        io->os_Flags = 0;
        for (i = 0; i < 4; i++) {
            io->os_Extra[i] = extra[i];
            if (buf[i].out) {
                io->os_Buf[i].ob_Data = buf[i].out;
                io->os_Flags |= 1UL << i;
            } else
                io->os_Buf[i].ob_Data = (APTR)buf[i].in;
            io->os_Buf[i].ob_Length = buf[i].in || buf[i].out ? buf[i].length : 0;
        }
        DoIO((struct IORequest *)io);
        status = io->os_Req.io_Error ? OSERR_LOST : io->os_Status;
        *result = io->os_Result;
        *aux = io->os_Aux;
        /* The service's answer says how much it wrote. */
        if (status == OSERR_OK && op == OTK_KEYGEN) {
            buf[0].written = io->os_Aux;
            buf[1].written = io->os_Result;
        } else if (status == OSERR_OK && op == OTK_DERIVE)
            buf[2].written = io->os_Result;
        DeleteIORequest((struct IORequest *)io);
    }
    if (port)
        DeleteMsgPort(port);
    return status == OSERR_OK ? OTK_OK : (int)status;
}

int opentls_amiga_open(void)
{
    static const char name[] = "opentls.key/1";
    if (opened)
        return (int)where;
    openPort = CreateMsgPort();
    opened = openPort ? (struct OSRequest *)CreateIORequest(openPort, sizeof *opened) : NULL;
    if (!opened || OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)opened, 0)) {
        if (opened)
            DeleteIORequest((struct IORequest *)opened);
        if (openPort)
            DeleteMsgPort(openPort);
        opened = NULL;
        openPort = NULL;
        return 0;
    }
    memset(opened->os_Buf, 0, sizeof opened->os_Buf);
    opened->os_Req.io_Command = OSCMD_OPEN;
    opened->os_Buf[0].ob_Data = (APTR)name;
    opened->os_Buf[0].ob_Length = sizeof name - 1;
    if (DoIO((struct IORequest *)opened) || opened->os_Status) {
        opentls_amiga_close();
        return 0;
    }
    handle = (UWORD)opened->os_Result;
    opened->os_Req.io_Command = OSCMD_WHERE;
    opened->os_Service = handle;
    DoIO((struct IORequest *)opened);
    where = opened->os_Result;
    if (!opentls_install(amigaTransport)) {
        opentls_amiga_close();
        return 0;
    }
    return (int)where;
}

void opentls_amiga_close(void)
{
    if (!opened)
        return;
    opentls_install(NULL);            /* anything still running does the work here */
    if (handle) {
        opened->os_Req.io_Command = OSCMD_CLOSE;
        opened->os_Service = handle;
        DoIO((struct IORequest *)opened);
    }
    CloseDevice((struct IORequest *)opened);
    DeleteIORequest((struct IORequest *)opened);
    DeleteMsgPort(openPort);
    opened = NULL;
    openPort = NULL;
    handle = 0;
    where = 0;
}
