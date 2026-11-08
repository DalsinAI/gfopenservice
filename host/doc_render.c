/*
 * doc.render/1 on the host (docs/DOC_RENDER.md): LibreOffice, or Apache
 * OpenOffice through office_pdf.py, turns the document into a PDF once (kept in a cache by the file's hash), then
 * poppler's pdfinfo, pdftoppm and pdftotext answer for its pages and text.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include "doc_render.h"
#include "blob.h"
#include "hostrun.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Pages larger than this are refused: 2^26 pixels is 256 MB of ARGB. */
#define MAX_PIXELS (1u << 26)
/* A page's own size, in pixels: 96 per inch. */
#define DPI 96

/* The office runs one conversion at a time per profile. */
static pthread_mutex_t convert_lock = PTHREAD_MUTEX_INITIALIZER;

/* The office program: $OPENSERVICE_OFFICE (a soffice, LibreOffice's or
 * OpenOffice's), else LibreOffice's soffice on the PATH, else OpenOffice in
 * /opt. OpenOffice has no --convert-to, so its own Python runs office_pdf.py
 * (beside this library) instead. */
static struct {
    char soffice[PATH_MAX];
    char python[PATH_MAX + 16];   /* set for OpenOffice */
    char script[PATH_MAX + 16];
} office;
static pthread_once_t office_once = PTHREAD_ONCE_INIT;

static int executable(const char *path)
{
    return path[0] && access(path, X_OK) == 0;
}

static void find_office(void)
{
    const char *env = getenv("OPENSERVICE_OFFICE"), *path = getenv("PATH");
    static const char *const opt[] = { "/opt/openoffice4/program/soffice", "/opt/openoffice.org3/program/soffice", NULL };
    char real[PATH_MAX], *slash;
    FILE *f;
    Dl_info me;
    int i;

    if (env && executable(env))
        snprintf(office.soffice, sizeof office.soffice, "%s", env);
    while (!office.soffice[0] && path && *path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char cand[PATH_MAX];
        if (len && len < sizeof cand - 9) {
            snprintf(cand, sizeof cand, "%.*s/soffice", (int)len, path);
            if (executable(cand))
                snprintf(office.soffice, sizeof office.soffice, "%s", cand);
        }
        path = end ? end + 1 : NULL;
    }
    for (i = 0; !office.soffice[0] && opt[i]; i++)
        if (executable(opt[i]))
            snprintf(office.soffice, sizeof office.soffice, "%s", opt[i]);
    if (!office.soffice[0] || !realpath(office.soffice, real) || !(slash = strrchr(real, '/')))
        return;
    /* OpenOffice says so in program/versionrc; LibreOffice says LibreOffice. */
    *slash = 0;
    {
        char rc[PATH_MAX + 16], line[256];
        int aoo = 0;
        snprintf(rc, sizeof rc, "%s/versionrc", real);
        if ((f = fopen(rc, "r"))) {
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "ProductSource=AOO", 17) || !strncmp(line, "ProductSource=OOO", 17))
                    aoo = 1;
            fclose(f);
        }
        if (!aoo)
            return;
        snprintf(office.python, sizeof office.python, "%s/python", real);
    }
    if (dladdr((void *)find_office, &me) && me.dli_fname && realpath(me.dli_fname, real)
        && (slash = strrchr(real, '/'))) {
        *slash = 0;
        snprintf(office.script, sizeof office.script, "%s/office_pdf.py", real);
    }
    if (!executable(office.python) || access(office.script, R_OK))
        office.soffice[0] = 0;             /* OpenOffice without its Python or the script: none */
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static const uint8_t *find(const uint8_t *d, uint32_t n, const void *what, uint32_t len)
{
    return len && n >= len ? memmem(d, n, what, len) : NULL;
}

/* An OLE2 stream name, as UTF-16LE. */
static int has_ole_name(const uint8_t *d, uint32_t n, const char *name)
{
    uint8_t w[64];
    uint32_t i, len = strlen(name);
    for (i = 0; i < len && i < 32; i++) {
        w[2 * i] = (uint8_t)name[i];
        w[2 * i + 1] = 0;
    }
    return find(d, n, w, 2 * len) != NULL;
}

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

uint32_t dr_sniff(const uint8_t *d, uint32_t n, uint32_t hint)
{
    if (n >= 5 && !memcmp(d, "%PDF-", 5))
        return FOURCC('P', 'D', 'F', ' ');
    if (n >= 5 && !memcmp(d, "{\\rtf", 5))
        return FOURCC('R', 'T', 'F', ' ');
    if (n >= 4 && !memcmp(d, "\xff" "WPC", 4))
        return FOURCC('W', 'P', 'D', ' ');
    if (n >= 4 && !memcmp(d, "\xc5\xd0\xd3\xc6", 4))       /* DOS EPS: PostScript with a preview */
        return FOURCC('E', 'P', 'S', ' ');
    if (n >= 4 && !memcmp(d, "%!PS", 4)) {
        const uint8_t *eol = memchr(d, '\n', n < 80 ? n : 80);
        return find(d, eol ? (uint32_t)(eol - d) : (n < 80 ? n : 80), "EPSF", 4)
            ? FOURCC('E', 'P', 'S', ' ') : FOURCC('P', 'S', ' ', ' ');
    }
    if (n >= 8 && !memcmp(d, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) {
        if (has_ole_name(d, n, "WordDocument"))
            return FOURCC('D', 'O', 'C', ' ');
        if (has_ole_name(d, n, "Workbook") || has_ole_name(d, n, "Book"))
            return FOURCC('X', 'L', 'S', ' ');
        if (has_ole_name(d, n, "PowerPoint Document"))
            return FOURCC('P', 'P', 'T', ' ');
        return 0;
    }
    if (n >= 4 && !memcmp(d, "PK\3\4", 4)) {
        if (n >= 38 && !memcmp(d + 30, "mimetype", 8)) {
            if (find(d, n < 200 ? n : 200, "opendocument.text", 17))
                return FOURCC('O', 'D', 'T', ' ');
            if (find(d, n < 200 ? n : 200, "opendocument.spreadsheet", 24))
                return FOURCC('O', 'D', 'S', ' ');
            if (find(d, n < 200 ? n : 200, "opendocument.presentation", 25))
                return FOURCC('O', 'D', 'P', ' ');
            if (find(d, n < 200 ? n : 200, "opendocument.graphics", 21))
                return FOURCC('O', 'D', 'G', ' ');
            if (find(d, n < 200 ? n : 200, "application/epub+zip", 20))
                return FOURCC('E', 'P', 'U', 'B');
            return 0;
        }
        if (find(d, n, "word/", 5))
            return FOURCC('D', 'O', 'C', 'X');
        if (find(d, n, "xl/", 3))
            return FOURCC('X', 'L', 'S', 'X');
        if (find(d, n, "ppt/", 4))
            return FOURCC('P', 'P', 'T', 'X');
        return 0;
    }
    /* Text with no signature: what the extension says. */
    switch (hint) {
    case FOURCC('C', 'S', 'V', ' '): case FOURCC('T', 'S', 'V', ' '): case FOURCC('M', 'D', ' ', ' '):
    case FOURCC('H', 'T', 'M', 'L'): case FOURCC('T', 'X', 'T', ' '):
        return hint;
    case FOURCC('H', 'T', 'M', ' '):
        return FOURCC('H', 'T', 'M', 'L');
    case FOURCC('M', 'A', 'R', 'K'):                   /* .markdown, cut to four letters */
        return FOURCC('M', 'D', ' ', ' ');
    }
    return 0;
}

static const char *extension(uint32_t f)
{
    switch (f) {
    case FOURCC('D', 'O', 'C', 'X'): return "docx";
    case FOURCC('X', 'L', 'S', 'X'): return "xlsx";
    case FOURCC('P', 'P', 'T', 'X'): return "pptx";
    case FOURCC('O', 'D', 'T', ' '): return "odt";
    case FOURCC('O', 'D', 'S', ' '): return "ods";
    case FOURCC('O', 'D', 'P', ' '): return "odp";
    case FOURCC('O', 'D', 'G', ' '): return "odg";
    case FOURCC('D', 'O', 'C', ' '): return "doc";
    case FOURCC('X', 'L', 'S', ' '): return "xls";
    case FOURCC('P', 'P', 'T', ' '): return "ppt";
    case FOURCC('R', 'T', 'F', ' '): return "rtf";
    case FOURCC('W', 'P', 'D', ' '): return "wpd";
    case FOURCC('P', 'S', ' ', ' '): return "ps";
    case FOURCC('E', 'P', 'S', ' '): return "eps";
    case FOURCC('E', 'P', 'U', 'B'): return "epub";
    case FOURCC('C', 'S', 'V', ' '): return "csv";
    case FOURCC('T', 'S', 'V', ' '): return "tsv";
    case FOURCC('M', 'D', ' ', ' '): return "md";
    case FOURCC('H', 'T', 'M', 'L'): return "html";
    case FOURCC('T', 'X', 'T', ' '): return "txt";
    default: return "pdf";
    }
}

/* The document as a PDF in the cache (path into pdf); 0 on success. */
static int to_pdf(const uint8_t *d, uint32_t n, uint32_t format, char *pdf, size_t room)
{
    char dir[512], k[64], src[700], profile[640], profile_url[600];
    struct stat sb;
    FILE *f;
    int rc = -1;

    if (hr_cache_dir("doc", dir, sizeof dir))
        return -1;
    hr_key(d, n, k, sizeof k);
    snprintf(pdf, room, "%s/%s.pdf", dir, k);
    if (stat(pdf, &sb) == 0 && sb.st_size > 0)
        return 0;
    snprintf(src, sizeof src, "%s/%s.%s", dir, k, extension(format));
    if (!(f = fopen(src, "wb")))
        return -1;
    if (fwrite(d, 1, n, f) != n) {
        fclose(f);
        unlink(src);
        return -1;
    }
    fclose(f);
    if (format == FOURCC('P', 'D', 'F', ' '))
        return 0;                               /* src is the PDF */
    if (format == FOURCC('P', 'S', ' ', ' ') || format == FOURCC('E', 'P', 'S', ' ')) {
        /* Ghostscript, sandboxed; an EPS gets a page the size of its drawing. */
        char out[720];
        char *argv[] = { "gs", "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pdfwrite", "-dEPSCrop", out, src, NULL };
        snprintf(out, sizeof out, "-sOutputFile=%s", pdf);
        rc = hr_run(argv, -1);
        unlink(src);
        return rc == 0 && stat(pdf, &sb) == 0 && sb.st_size > 0 ? 0 : -1;
    }
    pthread_once(&office_once, find_office);
    if (!office.soffice[0]) {
        unlink(src);
        return -2;                              /* no office on this host */
    }
    if (format == FOURCC('E', 'P', 'U', 'B') || format == FOURCC('M', 'D', ' ', ' ')
        || (format == FOURCC('H', 'T', 'M', 'L') && office.python[0])) {
        /* pandoc reads e-books and Markdown (and HTML, which OpenOffice's
         * importer mangles); the office lays its DOCX out. */
        char docx[700];
        char *argv[] = { "pandoc", src, "-o", docx, NULL };
        snprintf(docx, sizeof docx, "%s/%s.docx", dir, k);
        rc = hr_run(argv, -1);
        unlink(src);
        if (rc)
            return -1;
        snprintf(src, sizeof src, "%s", docx);
    }
    /* The two offices' profiles do not mix. */
    snprintf(profile_url, sizeof profile_url, "file://%s/%s", dir, office.python[0] ? "profile-aoo" : "profile");
    snprintf(profile, sizeof profile, "-env:UserInstallation=%s", profile_url);
    pthread_mutex_lock(&convert_lock);
    {
        /* CSV: comma (or tab), double quotes, UTF-8, from the first line. */
        int sheet = format == FOURCC('C', 'S', 'V', ' ') || format == FOURCC('T', 'S', 'V', ' ');
        char *options = format == FOURCC('T', 'S', 'V', ' ') ? "9,34,76,1" : "44,34,76,1";
        if (office.python[0]) {
            char *argv[] = { office.python, office.script, office.soffice, profile_url, src, pdf,
                             sheet ? options : NULL, NULL };
            rc = hr_run(argv, -1);
        } else {
            char csv[32];
            char *argv[] = { office.soffice, profile, "--headless", "--norestore", "--nolockcheck",
                             "--convert-to", "pdf", "--outdir", dir, src, NULL, NULL };
            snprintf(csv, sizeof csv, "--infilter=CSV:%s", options);
            if (sheet) {
                memmove(&argv[5], &argv[4], 6 * sizeof argv[0]);
                argv[4] = csv;
            }
            rc = hr_run(argv, -1);
        }
    }
    pthread_mutex_unlock(&convert_lock);
    unlink(src);
    return rc == 0 && stat(pdf, &sb) == 0 && sb.st_size > 0 ? 0 : -1;
}

/* Pages, and the first page's size in points, from pdfinfo. */
static int pdf_info(const char *pdf, uint32_t *pages, double *wpt, double *hpt)
{
    char *argv[] = { "pdfinfo", (char *)pdf, NULL };
    char *out = hr_run_read(argv, NULL), *p;
    int ok = 0;

    if (!out)
        return -1;
    if ((p = strstr(out, "\nPages:")) && sscanf(p + 7, "%u", pages) == 1
        && (p = strstr(out, "\nPage size:")) && sscanf(p + 11, "%lf x %lf", wpt, hpt) == 2
        && *pages > 0 && *wpt > 0 && *hpt > 0)
        ok = 1;
    free(out);
    return ok ? 0 : -1;
}

static void fit(uint32_t w, uint32_t h, uint32_t maxw, uint32_t maxh, uint32_t *ow, uint32_t *oh)
{
    *ow = w;
    *oh = h;
    if (maxw && *ow > maxw) {
        *oh = (uint32_t)((uint64_t)*oh * maxw / *ow);
        *ow = maxw;
    }
    if (maxh && *oh > maxh) {
        *ow = (uint32_t)((uint64_t)*ow * maxh / *oh);
        *oh = maxh;
    }
    if (!*ow)
        *ow = 1;
    if (!*oh)
        *oh = 1;
}

/* Page (0-based) at w x h into argb, through pdftoppm's PPM. */
static int render(const char *pdf, uint32_t page, uint32_t w, uint32_t h, uint8_t *argb)
{
    char first[16], sw[16], sh[16];
    char *argv[] = { "pdftoppm", "-f", first, "-l", first, "-scale-to-x", sw, "-scale-to-y", sh,
                     "-singlefile", "-aa", "yes", "-aaVector", "yes", (char *)pdf, NULL };
    size_t len = 0, at;
    uint32_t pw, ph, maxv, i;
    char *ppm;
    int consumed = 0;

    snprintf(first, sizeof first, "%u", page + 1);
    snprintf(sw, sizeof sw, "%u", w);
    snprintf(sh, sizeof sh, "%u", h);
    if (!(ppm = hr_run_read(argv, &len)))
        return -1;
    if (sscanf(ppm, "P6 %u %u %u%n", &pw, &ph, &maxv, &consumed) != 3 || pw != w || ph != h || maxv != 255) {
        free(ppm);
        return -1;
    }
    at = consumed + 1;                          /* one whitespace byte after maxval */
    if (len < at + (size_t)w * h * 3) {
        free(ppm);
        return -1;
    }
    for (i = 0; i < w * h; i++) {
        argb[4 * i] = 255;
        memcpy(argb + 4 * i + 1, ppm + at + 3 * i, 3);
    }
    free(ppm);
    return 0;
}

/* The text through pdftotext, as ISO-8859-1. */
static char *text(const char *pdf, size_t *len)
{
    char *argv[] = { "pdftotext", "-enc", "Latin1", "-layout", (char *)pdf, "-", NULL };
    return hr_run_read(argv, len);
}

/* The extension hint in capitals, space-padded: 'csv ' -> 'CSV '. */
static uint32_t upper(uint32_t hint)
{
    uint32_t out = 0;
    int i;
    for (i = 24; i >= 0; i -= 8) {
        uint8_t c = hint >> i;
        out = out << 8 | (c >= 'a' && c <= 'z' ? c - 32 : c ? c : ' ');
    }
    return out;
}

static int call_one(uint16_t op, uint32_t arg, const uint32_t extra[4], struct dr_buffer buf[4],
                    uint32_t *result, uint32_t *aux)
{
    char pdf[700];
    uint32_t format, pages, pw, ph, ow, oh;
    int st;
    double wpt, hpt;

    *result = *aux = 0;
    if (!buf[0].in || !buf[0].length || !(format = dr_sniff(buf[0].in, buf[0].length, upper(extra[2]))))
        return DR_BADREQUEST;
    if (!buf[1].out)
        return DR_TOOSMALL;
    if (op == DR_PROBE && buf[1].length < 24)
        return DR_TOOSMALL;
    if (op != DR_PROBE && op != DR_RENDER && op != DR_TEXT)
        return DR_BADREQUEST;
    if ((st = to_pdf(buf[0].in, buf[0].length, format, pdf, sizeof pdf)))
        return st == -2 ? DR_HOSTERROR : DR_BADREQUEST;

    if (op == DR_TEXT) {
        size_t len = 0;
        char *t = text(pdf, &len);
        if (!t)
            return DR_HOSTERROR;
        if (len > buf[1].length) {
            *result = (uint32_t)len;
            free(t);
            return DR_TOOSMALL;
        }
        memcpy(buf[1].out, t, len);
        free(t);
        buf[1].written = (uint32_t)len;
        *result = (uint32_t)len;
        return DR_OK;
    }

    if (pdf_info(pdf, &pages, &wpt, &hpt))
        return DR_BADREQUEST;
    pw = (uint32_t)(wpt * DPI / 72 + 0.5);
    ph = (uint32_t)(hpt * DPI / 72 + 0.5);
    fit(pw, ph, extra[0], extra[1], &ow, &oh);
    if ((uint64_t)ow * oh > MAX_PIXELS)
        return DR_BADREQUEST;

    if (op == DR_PROBE) {
        put32(buf[1].out, DR_KIND_DOCUMENT);
        put32(buf[1].out + 4, format);
        put32(buf[1].out + 8, 0);
        put32(buf[1].out + 12, pages);
        put32(buf[1].out + 16, ow);
        put32(buf[1].out + 20, oh);
        buf[1].written = 24;
        *result = pages;
        return DR_OK;
    }
    if (arg >= pages)
        return DR_BADREQUEST;
    if ((uint64_t)ow * oh * 4 > buf[1].length)
        return DR_TOOSMALL;
    if (render(pdf, arg, ow, oh, buf[1].out))
        return DR_HOSTERROR;
    buf[1].written = ow * oh * 4;
    *result = ow;
    *aux = oh;
    return DR_OK;
}

/* Files sent once (blob.h): the file named in extra[3] when buf0 is empty,
 * kept with KEEP (its handle to buf3), let go with DROP or RELEASE. */
int dr_call(uint16_t op, uint32_t arg, const uint32_t extra[4], struct dr_buffer buf[4],
            uint32_t *result, uint32_t *aux)
{
    struct dr_buffer sent = buf[0];
    struct blob_use use;
    uint32_t ex[4];
    int st;

    *result = *aux = 0;
    if (op == DR_RELEASE)
        return blob_release(arg);
    if (!buf[0].in)
        buf[0].length = 0;
    if ((st = blob_before(extra[3], &buf[0].in, &buf[0].length, &use)) != 0)
        return st;
    memcpy(ex, extra, sizeof ex);
    ex[3] &= ~(BLOB_KEEP | BLOB_DROP | ~(uint32_t)0 << BLOB_SHIFT);
    st = call_one(op, arg, ex, buf, result, aux);
    blob_after(st == DR_OK, extra[3], buf[0].in, buf[0].length, buf[3].out, buf[3].length, &buf[3].written, &use);
    buf[0] = sent;
    return st;
}
