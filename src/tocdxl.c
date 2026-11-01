/*
 * ToCDXL: turns a movie (MP4, MKV, AVI, WMV, WebM, MPEG...) into a CDXL file
 * for this Amiga, through the media.cdxl/1 service on the services card or a
 * paired Cradle (docs/MEDIA_CDXL.md). The movie is read into memory and sent
 * once; the CDXL comes back in pieces and is written as it arrives.
 *
 * Usage: ToCDXL FROM/A TO/A PRESET/K SIZE/K FPS/K/N PLANES/K/N RATE/K/N
 *               STEREO/S NOSOUND/S
 *   PRESET: ECS (32 colours, default on ECS), ECSHAM (HAM6), AGA (256 colours,
 *   default on AGA), AGAHAM (HAM8) or RTG (24-bit chunky, 640 x 360).
 *   SIZE: the largest picture, e.g. 320x256. The rest override the preset.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "devices/gfopenservice.h"

const char version[] __attribute__((used)) = "$VER: ToCDXL 0.1 (5.10.2026)";

#define CX_CONVERT 1
#define CX_READ    2
#define CX_CLOSE   3
#define CX_NO_SOUND (1UL << 30)
#define CX_STEREO   (1UL << 31)
#define PIECE 65536

static LONG run(struct OSRequest *io, UWORD command)
{
    io->os_Req.io_Command = command;
    DoIO((struct IORequest *)io);
    return io->os_Req.io_Error ? -100 - io->os_Req.io_Error : io->os_Status;
}

static ULONG get32(const UBYTE *p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

static int preset_of(const char *s)
{
    static const char *const names[] = { "ECS", "ECSHAM", "AGA", "AGAHAM", "RTG" };
    int i, k;
    for (i = 0; i < 5; i++) {
        for (k = 0; s[k] && names[i][k] && (s[k] & ~0x20) == names[i][k]; k++)
            ;
        if (!s[k] && !names[i][k])
            return i;
    }
    return -1;
}

/* The AGA chipset: Alice. */
static int aga(void)
{
    struct GfxBase *gfx = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    int yes = gfx && (gfx->ChipRevBits0 & GFXF_AA_ALICE);
    if (gfx)
        CloseLibrary((struct Library *)gfx);
    return yes;
}

int main(void)
{
    enum { FROM, TO, PRESET, SIZE, FPS, PLANES, RATE, STEREO, NOSOUND, NARGS };
    LONG args[NARGS];
    struct RDArgs *rd;
    struct MsgPort *port = NULL;
    struct OSRequest *io = NULL;
    BPTR in = 0, out = 0;
    UBYTE *movie = NULL, *piece = NULL, info[24];
    ULONG size = 0, total, done = 0, handle = 0, w = 0, h = 0;
    int preset, rc = RETURN_FAIL, opened = 0;
    UWORD service = 0;

    memset(args, 0, sizeof args);
    if (!(rd = ReadArgs((CONST_STRPTR)"FROM/A,TO/A,PRESET/K,SIZE/K,FPS/K/N,PLANES/K/N,RATE/K/N,STEREO/S,NOSOUND/S",
                        args, NULL))) {
        PrintFault(IoErr(), (CONST_STRPTR)"ToCDXL");
        return RETURN_FAIL;
    }
    /* Without PRESET: what this machine shows best without RTG. */
    preset = args[PRESET] ? preset_of((const char *)args[PRESET]) : aga() ? 2 : 0;
    if (args[SIZE]) {
        unsigned long sw = 0, sh = 0;
        if (sscanf((const char *)args[SIZE], "%lux%lu", &sw, &sh) != 2)
            preset = -1;
        w = (ULONG)sw;
        h = (ULONG)sh;
    }
    if (preset < 0) {
        printf("ToCDXL: PRESET is ECS, ECSHAM, AGA, AGAHAM or RTG; SIZE is like 320x256\n");
        goto done;
    }

    if (!(in = Open((CONST_STRPTR)args[FROM], MODE_OLDFILE))) {
        PrintFault(IoErr(), (CONST_STRPTR)args[FROM]);
        goto done;
    }
    Seek(in, 0, OFFSET_END);
    size = (ULONG)Seek(in, 0, OFFSET_BEGINNING);
    if (!size || !(movie = AllocVec(size, MEMF_ANY)) || !(piece = AllocVec(PIECE, MEMF_ANY))) {
        printf("ToCDXL: not enough memory for a %lu-byte movie\n", (unsigned long)size);
        goto done;
    }
    if ((ULONG)Read(in, movie, size) != size) {
        PrintFault(IoErr(), (CONST_STRPTR)args[FROM]);
        goto done;
    }

    if (!(port = CreateMsgPort()) || !(io = (struct OSRequest *)CreateIORequest(port, sizeof *io)))
        goto done;
    if (OpenDevice((CONST_STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)io, 0)) {
        printf("ToCDXL: cannot open %s (GFOpenUp's GFOpenService part installs it)\n", OPENSERVICE_NAME);
        goto done;
    }
    opened = 1;
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = (APTR)"media.cdxl/1";
    io->os_Buf[0].ob_Length = 12;
    if (run(io, OSCMD_OPEN)) {
        printf("ToCDXL: no services card or paired Cradle offers media.cdxl/1\n");
        goto done;
    }
    service = (UWORD)io->os_Result;

    io->os_Service = service;
    run(io, OSCMD_WHERE);
    printf("Converting %s on the %s...\n", (const char *)args[FROM],
           io->os_Result == OSWHERE_CARD ? "services card" : "Cradle");
    memset(io->os_Buf, 0, sizeof io->os_Buf);
    io->os_Service = service;
    io->os_Op = CX_CONVERT;
    io->os_Flags = 2;
    io->os_Arg = 0;
    io->os_Buf[0].ob_Data = movie;
    io->os_Buf[0].ob_Length = size;
    io->os_Buf[1].ob_Data = info;
    io->os_Buf[1].ob_Length = sizeof info;
    io->os_Extra[0] = (ULONG)preset;
    io->os_Extra[1] = w << 16 | (h & 0xffff);
    io->os_Extra[2] = (args[FPS] ? *(LONG *)args[FPS] & 0xff : 0) | (args[PLANES] ? (*(LONG *)args[PLANES] & 0xff) << 8 : 0);
    io->os_Extra[3] = (args[RATE] ? *(LONG *)args[RATE] & 0x3ffff : 0) | (args[STEREO] ? CX_STEREO : 0)
                      | (args[NOSOUND] ? CX_NO_SOUND : 0);
    if (run(io, OSCMD_CALL)) {
        printf("ToCDXL: the movie could not be converted (status %ld)\n", (long)io->os_Status);
        goto done;
    }
    handle = io->os_Result;
    total = io->os_Aux;
    FreeVec(movie);
    movie = NULL;

    if (!(out = Open((CONST_STRPTR)args[TO], MODE_NEWFILE))) {
        PrintFault(IoErr(), (CONST_STRPTR)args[TO]);
        goto done;
    }
    while (done < total) {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            printf("***Break\n");
            goto done;
        }
        memset(io->os_Buf, 0, sizeof io->os_Buf);
        io->os_Service = service;
        io->os_Op = CX_READ;
        io->os_Flags = 2;
        io->os_Arg = handle;
        io->os_Extra[0] = done;
        io->os_Buf[1].ob_Data = piece;
        io->os_Buf[1].ob_Length = PIECE;
        if (run(io, OSCMD_CALL) || !io->os_Result) {
            printf("ToCDXL: reading the CDXL stopped at %lu of %lu bytes\n", (unsigned long)done, (unsigned long)total);
            goto done;
        }
        if (Write(out, piece, io->os_Result) != (LONG)io->os_Result) {
            PrintFault(IoErr(), (CONST_STRPTR)args[TO]);
            goto done;
        }
        done += io->os_Result;
    }
    {
        static const char *const modes[] = { "colours", "HAM", "24-bit chunky" };
        ULONG m = get32(info + 16), r = get32(info + 20);
        printf("%s: %lu frames, %lux%lu, %s (%lu planes), %lu fps, ", (const char *)args[TO],
               (unsigned long)get32(info + 4), (unsigned long)get32(info + 8), (unsigned long)get32(info + 12),
               modes[(m >> 8) & 3], (unsigned long)(m & 0xff), (unsigned long)(r >> 24));
        if (r & 0xffffff)
            printf("%lu Hz %s, ", (unsigned long)(r & 0xffffff), (m >> 16) & 1 ? "stereo" : "mono");
        else
            printf("no sound, ");
        printf("%lu bytes\n", (unsigned long)total);
    }
    rc = RETURN_OK;
done:
    if (handle) {
        memset(io->os_Buf, 0, sizeof io->os_Buf);
        io->os_Service = service;
        io->os_Op = CX_CLOSE;
        io->os_Flags = 0;
        io->os_Arg = handle;
        run(io, OSCMD_CALL);
    }
    if (service) {
        io->os_Service = service;
        run(io, OSCMD_CLOSE);
    }
    if (opened)
        CloseDevice((struct IORequest *)io);
    if (io)
        DeleteIORequest((struct IORequest *)io);
    if (port)
        DeleteMsgPort(port);
    if (out)
        Close(out);
    if (in)
        Close(in);
    if (piece)
        FreeVec(piece);
    if (movie)
        FreeVec(movie);
    FreeArgs(rd);
    return rc;
}
