/*
 * gfopenservice.device: hand work to a named service.
 *
 * A program opens the device, asks for a service by name and version
 * ("opentls.key/1", "echo/1"), and sends it requests. The device takes each
 * request to the services card when the machine has one (AmiCradle on a
 * PC, our appliance), or else to a paired Cradle on the LAN that offers the
 * service. When neither has it, OSCMD_OPEN fails with OSERR_NOSERVICE and the
 * program does the work itself: the device never does it on the 68k.
 *
 *   struct MsgPort *port = CreateMsgPort();
 *   struct OSRequest *io = (struct OSRequest *)CreateIORequest(port, sizeof *io);
 *   OpenDevice(OPENSERVICE_NAME, 0, (struct IORequest *)io, 0);
 *   io->os_Req.io_Command = OSCMD_OPEN;
 *   io->os_Buf[0].ob_Data = "opentls.key/1";
 *   io->os_Buf[0].ob_Length = 13;
 *   DoIO((struct IORequest *)io);         -> os_Status 0, os_Result the handle
 *   io->os_Req.io_Command = OSCMD_CALL;
 *   io->os_Service = handle; io->os_Op = ...; buffers ...
 *   SendIO(...) / WaitIO(...) / AbortIO(...)
 *
 * Requests run at the same time; each completes on its own. The fields
 * mirror one entry of the services card (design/SERVICES_CARD.md, v1).
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef DEVICES_OPENSERVICE_H
#define DEVICES_OPENSERVICE_H

#include <exec/io.h>

#define OPENSERVICE_NAME "gfopenservice.device"

#define OSCMD_CALL  (CMD_NONSTD + 0)   /* os_Service, os_Op, os_Flags, os_Arg, buffers, os_Extra */
#define OSCMD_OPEN  (CMD_NONSTD + 1)   /* os_Buf[0]: the name; os_Result: the handle */
#define OSCMD_CLOSE (CMD_NONSTD + 2)   /* os_Service: the handle */
#define OSCMD_LIST  (CMD_NONSTD + 3)   /* os_Buf[0] (out): NUL-separated names; os_Result: bytes */
#define OSCMD_WHERE (CMD_NONSTD + 4)   /* os_Service: the handle; os_Result: OSWHERE_* */

#define OSWHERE_CARD 1   /* the services card: this machine's host */
#define OSWHERE_LAN  2   /* a Cradle in the nursery */

/* os_Status, as the card's completion status. */
#define OSERR_OK          0
#define OSERR_NOSERVICE (-1)
#define OSERR_BADREQUEST (-2)
#define OSERR_CANCELLED (-3)
#define OSERR_TOOSMALL  (-4)
#define OSERR_HOST      (-5)
#define OSERR_LOST      (-6)   /* the device lost the card or the Cradle */

struct OSBuffer {
    APTR ob_Data;
    ULONG ob_Length;           /* 0: unused */
};

struct OSRequest {
    struct IORequest os_Req;
    UWORD os_Service;          /* a handle from OSCMD_OPEN */
    UWORD os_Op;               /* the service's operation */
    ULONG os_Flags;            /* bit i set: buffer i is written by the service */
    ULONG os_Arg;
    struct OSBuffer os_Buf[4];
    ULONG os_Extra[4];         /* operation-specific */
    LONG os_Status;            /* out: OSERR_* or the service's own (negative) */
    ULONG os_Result;           /* out */
    ULONG os_Aux;              /* out */
    ULONG os_Private[4];       /* the device's */
};

#endif
