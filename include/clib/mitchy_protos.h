/* Automatically generated header (sfdc 1.12)! Do not edit! */

#ifndef CLIB_MITCHY_PROTOS_H
#define CLIB_MITCHY_PROTOS_H

/*
**   $VER: mitchy_protos.h 1.0 $Id: mitchy_lib.sfd 1.0 (9.11.2026) $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2026 AmiCradle Team, MIT licence.
**       (made by sfdc from sfd/mitchy_lib.sfd)
*/

#include <exec/types.h>
#include <exec/ports.h>
#include <libraries/configvars.h>
#include <libraries/mitchy.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* mitchy.library: named services on Mitchy and the ACSV boards. MIT, (c) 2026 AmiCradle Team */
struct MitchyService * MitchyOpen(CONST_STRPTR name, ULONG flags, LONG * error);
VOID MitchyClose(struct MitchyService * service);
LONG MitchyWhere(struct MitchyService * service);
struct MitchyRequest * MitchyAllocRequest(struct MitchyService * service, struct MsgPort * port);
VOID MitchyFreeRequest(struct MitchyRequest * request);
LONG MitchyDoRequest(struct MitchyRequest * request);
LONG MitchySendRequest(struct MitchyRequest * request);
BOOL MitchyCheckRequest(struct MitchyRequest * request);
LONG MitchyWaitRequest(struct MitchyRequest * request);
LONG MitchyAbortRequest(struct MitchyRequest * request);
LONG MitchyCall(struct MitchyService * service, ULONG op, ULONG flags, ULONG arg, struct MitchyBuffer * buffers, ULONG * extra, ULONG * resultAux);
LONG MitchyList(STRPTR buffer, ULONG size);
struct ConfigDev * MitchyFindBoard(struct ConfigDev * previous, ULONG boardClass);
CONST_STRPTR MitchyStatusText(LONG status);
struct MitchyRequest * MitchyRequestFromMsg(struct Message * message);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_MITCHY_PROTOS_H */
