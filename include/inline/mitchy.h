/* Automatically generated header (sfdc 1.12)! Do not edit! */

#ifndef _INLINE_MITCHY_H
#define _INLINE_MITCHY_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef MITCHY_BASE_NAME
#define MITCHY_BASE_NAME MitchyBase
#endif /* !MITCHY_BASE_NAME */

#define MitchyOpen(___name, ___flags, ___error) \
      LP3(0x1e, struct MitchyService *, MitchyOpen , CONST_STRPTR, ___name, a0, ULONG, ___flags, d0, LONG *, ___error, a1,\
      , MITCHY_BASE_NAME)

#define MitchyClose(___service) \
      LP1NR(0x24, MitchyClose , struct MitchyService *, ___service, a0,\
      , MITCHY_BASE_NAME)

#define MitchyWhere(___service) \
      LP1(0x2a, LONG, MitchyWhere , struct MitchyService *, ___service, a0,\
      , MITCHY_BASE_NAME)

#define MitchyAllocRequest(___service, ___port) \
      LP2(0x30, struct MitchyRequest *, MitchyAllocRequest , struct MitchyService *, ___service, a0, struct MsgPort *, ___port, a1,\
      , MITCHY_BASE_NAME)

#define MitchyFreeRequest(___request) \
      LP1NR(0x36, MitchyFreeRequest , struct MitchyRequest *, ___request, a0,\
      , MITCHY_BASE_NAME)

#define MitchyDoRequest(___request) \
      LP1(0x3c, LONG, MitchyDoRequest , struct MitchyRequest *, ___request, a0,\
      , MITCHY_BASE_NAME)

#define MitchySendRequest(___request) \
      LP1(0x42, LONG, MitchySendRequest , struct MitchyRequest *, ___request, a0,\
      , MITCHY_BASE_NAME)

#define MitchyCheckRequest(___request) \
      LP1(0x48, BOOL, MitchyCheckRequest , struct MitchyRequest *, ___request, a0,\
      , MITCHY_BASE_NAME)

#define MitchyWaitRequest(___request) \
      LP1(0x4e, LONG, MitchyWaitRequest , struct MitchyRequest *, ___request, a0,\
      , MITCHY_BASE_NAME)

#define MitchyAbortRequest(___request) \
      LP1(0x54, LONG, MitchyAbortRequest , struct MitchyRequest *, ___request, a0,\
      , MITCHY_BASE_NAME)

#define MitchyCall(___service, ___op, ___flags, ___arg, ___buffers, ___extra, ___resultAux) \
      LP7(0x5a, LONG, MitchyCall , struct MitchyService *, ___service, a0, ULONG, ___op, d0, ULONG, ___flags, d1, ULONG, ___arg, d2, struct MitchyBuffer *, ___buffers, a1, ULONG *, ___extra, a2, ULONG *, ___resultAux, a3,\
      , MITCHY_BASE_NAME)

#define MitchyList(___buffer, ___size) \
      LP2(0x60, LONG, MitchyList , STRPTR, ___buffer, a0, ULONG, ___size, d0,\
      , MITCHY_BASE_NAME)

#define MitchyFindBoard(___previous, ___boardClass) \
      LP2(0x66, struct ConfigDev *, MitchyFindBoard , struct ConfigDev *, ___previous, a0, ULONG, ___boardClass, d0,\
      , MITCHY_BASE_NAME)

#define MitchyStatusText(___status) \
      LP1(0x6c, CONST_STRPTR, MitchyStatusText , LONG, ___status, d0,\
      , MITCHY_BASE_NAME)

#define MitchyRequestFromMsg(___message) \
      LP1(0x72, struct MitchyRequest *, MitchyRequestFromMsg , struct Message *, ___message, a0,\
      , MITCHY_BASE_NAME)

#endif /* !_INLINE_MITCHY_H */
