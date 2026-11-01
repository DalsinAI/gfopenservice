/*
 * The Cradles this Amiga has paired with, and its own identity
 * (openservice.h). Plain dos.library, so the device can use it too.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include <string.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "openservice.h"

static int hexValue(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int openservice_hex(const char *text, unsigned char *out, int length)
{
    int i;
    for (i = 0; i < length; i++) {
        int hi = hexValue(text[2 * i]), lo = hi < 0 ? -1 : hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 1;
}

/* The line for fingerprint: "fp", or "fp key name" once paired with a code. */
static int findLine(const char *fingerprint, char *line, int size)
{
    int found = 0;
    BPTR f;
    if (!fingerprint || !fingerprint[0])
        return 0;
    f = Open((CONST_STRPTR)OPENSERVICE_PAIRED_FILE, MODE_OLDFILE);
    if (!f)
        return 0;
    while (!found && FGets(f, (STRPTR)line, size)) {
        size_t n = strcspn(line, " \t\r\n");
        found = n == strlen(fingerprint) && !strncmp(line, fingerprint, n);
    }
    Close(f);
    return found;
}

int openservice_paired(const char *fingerprint)
{
    char line[160];
    return findLine(fingerprint, line, sizeof line);
}

int openservice_psk(const char *fingerprint, unsigned char key[32])
{
    char line[160], *p;
    if (!findLine(fingerprint, line, sizeof line))
        return 0;
    p = line + strcspn(line, " \t\r\n");
    while (*p == ' ' || *p == '\t')
        p++;
    return strlen(p) >= 64 && openservice_hex(p, key, 32);
}

int openservice_amiga_id(unsigned char id[16])
{
    char text[40];
    BPTR f = Open((CONST_STRPTR)OPENSERVICE_ID_FILE, MODE_OLDFILE);
    int ok = 0;
    if (f) {
        ok = FGets(f, (STRPTR)text, sizeof text) && strlen(text) >= 32 && openservice_hex(text, id, 16);
        Close(f);
    }
    return ok;
}
