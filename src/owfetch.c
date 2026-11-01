/*
 * OWFetch: fetches a URL through openweb.fetch (docs/OPENWEB_FETCH.md),
 * on the services card or a paired Cradle (the network and TLS on the x86
 * or ARM64 cores), or when neither is there, natively over this Amiga's
 * TCP/IP stack and OpenTLS. Prints the status line and the headers, then
 * writes the body to TO, or to the console.
 *
 * Usage: OWFetch URL/A TO/K HEAD/S HEADER/M METHOD/K FROM/K TIMEOUT/K/N
 *                RAW/S QUIET/S INFO/S NATIVE/S
 *   NATIVE: this Amiga's own TCP/IP and OpenTLS, even with a card there.
 *   HEADER: one or more "Name: value", sent as they are (Cookie, Accept...).
 *   HEAD: the head only. FROM: a file sent as the body (POST unless METHOD).
 *   RAW: the body as the server sent it (otherwise gzip, deflate and br are
 *        undone; natively by the datatypes, when they are installed).
 *   QUIET: no head printed. INFO: the TLS summary too.
 * Ctrl-C cancels. Returns 0 for any HTTP answer below 400, 5 for 400 and
 * above, 10 when the fetch failed, 20 when it could not start.
 *
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "openweb/webfetch.h"

const char version[] __attribute__((used)) = "$VER: OWFetch 0.2 (23.10.2026)";

/* A TLS handshake wants 16 KB of stack free (opentls.library). */
unsigned long __stack = 65536;

#define PIECE (256UL << 10)
#define HEAD_ROOM (72UL << 10)
#define MAX_HEADERS 32

static BOOL broken(void)
{
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

/* The info text's lines, each with a leading "  ". */
static void print_info(const struct wf_head *h)
{
    ULONG i = 0;
    while (i < h->info_length) {
        ULONG k = i;
        while (k < h->info_length && h->info[k] != '\n')
            k++;
        printf("  %.*s\n", (int)(k - i), h->info + i);
        i = k + 1;
    }
}

/* One key's value from the info text ("error"), into out. */
static void info_value(const struct wf_head *h, const char *key, char *out, ULONG room)
{
    ULONG i = 0, n = strlen(key);
    out[0] = 0;
    while (i < h->info_length) {
        ULONG k = i;
        while (k < h->info_length && h->info[k] != '\n')
            k++;
        if (k - i > n && !memcmp(h->info + i, key, n) && h->info[i + n] == '=') {
            ULONG m = k - i - n - 1 < room - 1 ? k - i - n - 1 : room - 1;
            memcpy(out, h->info + i + n + 1, m);
            out[m] = 0;
            return;
        }
        i = k + 1;
    }
}

int main(void)
{
    enum { URL, TO, HEAD, HEADER, METHOD, FROM, TIMEOUT, RAW, QUIET, INFO, NATIVE, NARGS };
    LONG args[NARGS];
    struct RDArgs *rd;
    struct wf_service s;
    struct wf_head h;
    const char *method, *headers[MAX_HEADERS + 1];
    char *request = NULL, why[200];
    UBYTE *head = NULL, *piece = NULL;
    BPTR in = 0, out = 0;
    ULONG fetch = 0, flags, body_length = 0, part = 0, total = 0, got, end = 0, request_room;
    LONG status, count = 0, rc = RETURN_FAIL;
    BOOL opened = FALSE;

    memset(args, 0, sizeof args);
    memset(&s, 0, sizeof s);
    if (!(rd = ReadArgs((CONST_STRPTR)"URL/A,TO/K,HEAD/S,HEADER/M,METHOD/K,FROM/K,TIMEOUT/K/N,RAW/S,QUIET/S,INFO/S,NATIVE/S",
                        args, NULL))) {
        PrintFault(IoErr(), (CONST_STRPTR)"OWFetch");
        return RETURN_FAIL;
    }
    method = args[METHOD] ? (const char *)args[METHOD] : args[HEAD] ? "HEAD" : args[FROM] ? "POST" : "GET";
    if (args[HEADER])
        while (((const char **)args[HEADER])[count] && count < MAX_HEADERS) {
            headers[count] = ((const char **)args[HEADER])[count];
            count++;
        }
    request_room = strlen(method) + strlen((const char *)args[URL]) + 8;
    for (status = 0; status < count; status++)
        request_room += strlen(headers[status]) + 1;
    if (!(request = AllocVec(request_room, MEMF_ANY)) || !(head = AllocVec(HEAD_ROOM, MEMF_ANY))
        || !(piece = AllocVec(PIECE, MEMF_ANY))) {
        printf("OWFetch: not enough memory\n");
        goto done;
    }
    flags = args[RAW] ? 0 : WF_DECODE;
    if (args[FROM]) {
        if (!(in = Open((CONST_STRPTR)args[FROM], MODE_OLDFILE))) {
            PrintFault(IoErr(), (CONST_STRPTR)args[FROM]);
            goto done;
        }
        Seek(in, 0, OFFSET_END);
        body_length = (ULONG)Seek(in, 0, OFFSET_BEGINNING);
        part = body_length < PIECE ? body_length : PIECE;
        if (part && Read(in, piece, part) != (LONG)part) {
            PrintFault(IoErr(), (CONST_STRPTR)args[FROM]);
            goto done;
        }
        flags |= WF_BODY;
    }

    if ((status = wf_service_open_mode(&s, args[NATIVE] ? WF_MODE_NATIVE : WF_MODE_AUTO)) != OSERR_OK) {
        printf("OWFetch: no services card or paired Cradle offers %s, and no TCP/IP stack is running (%ld)\n",
               WF_SERVICE_NAME, (long)status);
        goto done;
    }
    wf_request(request, request_room, method, (const char *)args[URL], headers, count);
    if (args[INFO])
        printf("(%s)\n", s.where == WF_WHERE_NATIVE ? "native: this Amiga's TCP/IP and OpenTLS"
                          : s.where == OSWHERE_LAN ? "a paired Cradle" : "the services card");
    if ((status = wf_open(&s, request, 0, flags, body_length, args[TIMEOUT] ? (ULONG)*(LONG *)args[TIMEOUT] : 0,
                          piece, part, &fetch)) != OSERR_OK) {
        printf("OWFetch: the request was refused (%ld): check the URL and the headers\n", (long)status);
        goto done;
    }
    opened = TRUE;
    rc = RETURN_ERROR;

    /* The rest of the body, a piece at a time. */
    for (total = part; (flags & WF_BODY) && total < body_length; ) {
        ULONG n = body_length - total < PIECE ? body_length - total : PIECE, taken = 0;
        if (Read(in, piece, n) != (LONG)n) {
            PrintFault(IoErr(), (CONST_STRPTR)args[FROM]);
            goto done;
        }
        do {
            if (broken()) {
                printf("***Break\n");
                goto done;
            }
            if ((status = wf_send(&s, fetch, piece, n, total + n == body_length, 1000, &taken)) != OSERR_OK) {
                printf("OWFetch: sending the body failed (%ld)\n", (long)status);
                goto done;
            }
        } while (!taken);
        total += n;
    }

    while ((status = wf_head(&s, fetch, head, HEAD_ROOM, 500, &h)) == WF_NOTYET)
        if (broken()) {
            printf("***Break\n");
            goto done;
        }
    if (status != OSERR_OK) {
        printf("OWFetch: no head (%ld)\n", (long)status);
        goto done;
    }
    if (h.error) {
        info_value(&h, "error", why, sizeof why);
        printf("OWFetch: %s failed (%s): %s\n", (const char *)args[URL], wf_error_name(h.error), why);
        if (h.error == WF_E_TLS || args[INFO])
            print_info(&h);
        goto done;
    }
    if (!args[QUIET]) {
        printf("%.*s", (int)h.text_length, h.text);
        if (args[INFO] && h.info_length) {
            printf("(%s)\n", h.flags & WF_H_VERIFIED ? "TLS, certificate trusted" : "TLS");
            print_info(&h);
        }
        printf("\n");
    }
    rc = h.status >= 400 ? RETURN_WARN : RETURN_OK;
    if (args[HEAD] || (h.flags & WF_H_NOBODY))
        goto done;

    if (args[TO] && !(out = Open((CONST_STRPTR)args[TO], MODE_NEWFILE))) {
        PrintFault(IoErr(), (CONST_STRPTR)args[TO]);
        rc = RETURN_ERROR;
        goto done;
    }
    fflush(stdout);
    total = 0;
    do {
        if (broken()) {
            printf("***Break\n");
            rc = RETURN_ERROR;
            goto done;
        }
        if ((status = wf_read(&s, fetch, piece, PIECE, 1000, &got, &end)) != OSERR_OK) {
            printf("OWFetch: reading the body failed (%ld)\n", (long)status);
            rc = RETURN_ERROR;
            goto done;
        }
        if (got && Write(out ? out : Output(), piece, got) != (LONG)got) {
            PrintFault(IoErr(), (CONST_STRPTR)(args[TO] ? (const char *)args[TO] : "OWFetch"));
            rc = RETURN_ERROR;
            goto done;
        }
        total += got;
    } while (!end);
    if (end >> 8) {
        printf("OWFetch: the body stopped after %lu bytes (%s)\n", (unsigned long)total, wf_error_name(end >> 8));
        rc = RETURN_ERROR;
    } else if (args[TO] && !args[QUIET])
        printf("%lu bytes to %s\n", (unsigned long)total, (const char *)args[TO]);
done:
    if (opened) {
        if (rc == RETURN_ERROR)
            wf_cancel(&s, fetch);
        wf_close(&s, fetch);
    }
    wf_service_close(&s);
    if (out)
        Close(out);
    if (in)
        Close(in);
    if (piece)
        FreeVec(piece);
    if (head)
        FreeVec(head);
    if (request)
        FreeVec(request);
    FreeArgs(rd);
    return (int)rc;
}
