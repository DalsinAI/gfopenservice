/*
 * gfopenservice.device: the old name of mitchy.device, for programs built
 * with it. Its Open opens mitchy.device on the caller's own request, so the
 * request's io_Device is then mitchy.device's: every later call, the close
 * included, goes there, and nothing here is ever open.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <exec/types.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include "devices/mitchy.h"

#define REG(r, x) x __asm(#r)

struct ExecBase *SysBase;
static BPTR segList;

const char AliasName[] = OPENSERVICE_NAME;
const char AliasVersion[] __attribute__((used)) = "$VER: gfopenservice.device 2.0 (8.11.2026) now mitchy.device";
#define AliasIdString (AliasVersion + 6)

static struct Device *aliasInit(REG(d0, struct Device *dev), REG(a0, BPTR seg), REG(a6, struct ExecBase *sysBase))
{
    SysBase = sysBase;
    segList = seg;
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)AliasName;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = 2;
    dev->dd_Library.lib_Revision = 0;
    dev->dd_Library.lib_IdString = (APTR)AliasIdString;
    return dev;
}

static void aliasOpen(REG(a1, struct IORequest *io), REG(d0, ULONG unit), REG(d1, ULONG flags), REG(a6, struct Device *dev))
{
    (void)dev;
    if (OpenDevice((CONST_STRPTR)MITCHY_NAME, unit, io, flags) && !io->io_Error)
        io->io_Error = IOERR_OPENFAIL;
}

static BPTR aliasExpunge(REG(a6, struct Device *dev))
{
    BPTR seg = segList;
    Remove(&dev->dd_Library.lib_Node);
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize, dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
    return seg;
}

static ULONG aliasNull(void)
{
    return 0;
}

static const APTR funcTable[] = {
    (APTR)aliasOpen,
    (APTR)aliasNull,                    /* Close, BeginIO and AbortIO: only ever mitchy.device's */
    (APTR)aliasExpunge,
    (APTR)aliasNull,
    (APTR)aliasNull,
    (APTR)aliasNull,
    (APTR)-1
};

static const ULONG initTable[] = {
    sizeof(struct Device),
    (ULONG)funcTable,
    0,
    (ULONG)aliasInit
};

const struct Resident alias_romtag __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&alias_romtag,
    (APTR)(&alias_romtag + 1),
    RTF_AUTOINIT,
    2,
    NT_DEVICE,
    0,
    (char *)AliasName,
    (char *)AliasIdString,
    (APTR)initTable
};
