#ifndef LIBRARIES_MITCHY_H
#define LIBRARIES_MITCHY_H
/*
 * mitchy.library: named services for any program, on Mitchy (the services
 * card, ACSV), on the other AmiCradle Team boards that carry the ACSV block
 * (AkikoPlus, the CPU cores board), or on a paired Cradle on the LAN.
 *
 * A program opens a service by name and version ("echo/1",
 * "media.decode/1"), fills a MitchyRequest and sends it, waiting for it
 * (MitchyDoRequest) or not (MitchySendRequest, then a signal or a message).
 * The fields mirror one entry of the card's submission ring (AmiCradle's
 * docs/architecture/SERVICES_CARD.md, v1). The library goes through
 * gfopenservice.device, which owns the boards' rings; a program never
 * writes a board's registers itself.
 *
 * When neither a board nor a paired Cradle offers a service, MitchyOpen
 * fails (MERR_NOTFITTED when there is no ACSV board in the machine at all),
 * and the program does the work itself on the 68k.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_PORTS_H
#include <exec/ports.h>
#endif

#define MITCHYNAME    "mitchy.library"
#define MITCHYVERSION 1

/* The boards: AmiCradle Team ($DA15), with 'ACSV' at their base. */
#define MITCHY_MANUFACTURER   0xDA15
#define MITCHY_MAGIC          0x41435356UL    /* 'ACSV' */
#define MITCHY_PRODUCT_CARD   13              /* Mitchy, the services card */
#define MITCHY_PRODUCT_CORES  7               /* the CPU cores board */
#define MITCHY_PRODUCT_AKIKO  18              /* AkikoPlus, the media board */

/* A board's CLASS register ($20), for MitchyFindBoard. */
#define MITCHY_CLASS_ANY      0
#define MITCHY_CLASS_SERVICES 1               /* Mitchy */
#define MITCHY_CLASS_CORES    2
#define MITCHY_CLASS_FPU      3
#define MITCHY_CLASS_TPU      4
#define MITCHY_CLASS_MEDIA    5               /* AkikoPlus */

/* MitchyOpen's flags. */
#define MOF_BOARDONLY  (1UL << 0)             /* a board in this machine only, never a Cradle on the LAN */

/* MitchyWhere. */
#define MITCHY_WHERE_NONE   0
#define MITCHY_WHERE_BOARD  1                 /* a board in this machine (its host does the work) */
#define MITCHY_WHERE_LAN    2                 /* a paired Cradle on the LAN */

/* Statuses. 0 and -1 to -5 are the card's own completion statuses; a
 * service may answer with its own negative ones (the CPU cores board's
 * cpu.m68k/1: -6 fault, -7 timeout). The library's own are -100 on. */
#define MITCHY_OK          0
#define MERR_NOSERVICE   (-1)     /* no board or Cradle offers it; or the handle is not open there */
#define MERR_BADREQUEST  (-2)
#define MERR_CANCELLED   (-3)     /* MitchyAbortRequest */
#define MERR_TOOSMALL    (-4)     /* a written buffer is too small: mr_Result says how much is needed */
#define MERR_HOST        (-5)     /* the host failed or is busy (retry) */
#define MERR_LOST        (-6)     /* the device lost the board or the Cradle (as the device says it) */
#define MERR_NODEVICE  (-100)     /* gfopenservice.device would not open */
#define MERR_NOMEMORY  (-101)
#define MERR_BUSY      (-102)     /* the request is in flight already */
#define MERR_NOTFITTED (-103)     /* MitchyOpen: no ACSV board in this machine, and no paired Cradle offers it */
#define MERR_NOSIGNAL  (-104)     /* MitchyAllocRequest: no free signal in this task */

/* A request's buffers. A buffer whose bit is set in mr_Flags is written by
 * the service (it may write less: mr_Result usually says how much); the
 * others are only read. Length 0: unused. Any memory, Chip or Fast; the
 * device keeps the data cache right (CacheClearE) both ways. The buffers
 * one request marks written may total 16 MB at most. */
#define MBF_WRITTEN(i)  (1UL << (i))
#define MBF_BUF0        MBF_WRITTEN(0)
#define MBF_BUF1        MBF_WRITTEN(1)
#define MBF_BUF2        MBF_WRITTEN(2)
#define MBF_BUF3        MBF_WRITTEN(3)

struct MitchyBuffer {
    APTR  mb_Data;
    ULONG mb_Length;
};

/* An open service (MitchyOpen). Read only. */
struct MitchyService {
    UWORD ms_Handle;              /* the device's handle */
    UWORD ms_Where;               /* MITCHY_WHERE_* */
    UBYTE ms_Name[64];            /* as opened, NUL ended */
};

/* A request (MitchyAllocRequest; never allocate one yourself). Fill the
 * fields above mr_Status, then MitchyDoRequest or MitchySendRequest. The
 * library keeps its own part after these fields. */
struct MitchyRequest {
    struct MitchyService *mr_Service;   /* read only: the service it was made for */
    UWORD mr_Op;                  /* the service's operation */
    UWORD mr_Pad;
    ULONG mr_Flags;               /* MBF_WRITTEN(i) */
    ULONG mr_Arg;
    struct MitchyBuffer mr_Buf[4];
    ULONG mr_Extra[4];            /* operation-specific */
    /* out, once the request has completed (Do, Check, Wait or Abort): */
    LONG  mr_Status;              /* MITCHY_OK, MERR_* or the service's own */
    ULONG mr_Result;
    ULONG mr_Aux;
    /* read only: */
    ULONG mr_SigMask;             /* made with no port: the signal its completion sets, for Wait() */
    struct MsgPort *mr_ReplyPort; /* made with a port: where its completion message goes */
};

#endif
