"""Checks media_tool.c's TIFF and ZIP readers against offsets near 2^32 in
the file (an IFD at 0xFFFFFFFE, a central directory or local header at
0xFFFFFFF0), which once wrapped the bounds checks and read 4 GB past the
buffer inside the emulator. Builds media_tool.c with AddressSanitizer into a
small driver, so it needs gcc with libasan and zlib's headers, not libavif.
MIT, Copyright (c) 2026 AmigaChrome Team."""
import os
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media_tool.h"

static uint8_t *load(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long len;
    if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET))
        exit(2);
    d = malloc(len);                    /* exactly the file: ASan sees one byte past */
    if (!d || fread(d, 1, len, f) != (size_t)len)
        exit(2);
    fclose(f);
    *n = (uint32_t)len;
    return d;
}

int main(int argc, char **argv)
{
    uint32_t n, format = 0, len = 0;
    uint8_t *d = load(argv[2], &n), *pic = NULL;
    int r = -1;

    if (!strcmp(argv[1], "raw"))
        r = md_is_raw(d, n, "");
    else
        r = (pic = md_zip_picture(d, n, argv[1], &format, &len)) != NULL;
    printf("%d\n", r);
    free(pic);
    free(d);
    return 0;
}
"""


def tiff(order, ifd, tail=b""):
    if order == "II":
        return b"II*\0" + struct.pack("<I", ifd) + tail
    return b"MM\0*" + struct.pack(">I", ifd) + tail


def central(name, local, method=0, size=0):
    return (struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, method, 0, 0, 0, size, size,
                        len(name), 0, 0, 0, 0, 0, local) + name)


def localhdr(name, data):
    return (struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, 0, 0, 0, 0, len(data), len(data), len(name), 0)
            + name + data)


def eocd(entries, cd_size, cd_off):
    return struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, entries, entries, cd_size, cd_off, 0)


def comic(cd_off=None, local_off=None):
    """A comic book with one page, 'a.png', whose offsets can be replaced."""
    body = localhdr(b"pad.txt", b"x" * 40)       # md_zip_picture wants 58 bytes or more
    cd = central(b"a.png", 0 if local_off is None else local_off)
    return body + cd + eocd(1, len(cd), len(body) if cd_off is None else cd_off)


with tempfile.TemporaryDirectory() as tmp:
    exe = os.path.join(tmp, "mt")
    src = os.path.join(tmp, "mt.c")
    open(src, "w").write(DRIVER)
    subprocess.run(["gcc", "-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I" + HERE, "-o", exe, src, os.path.join(HERE, "media_tool.c"),
                    os.path.join(HERE, "hostrun.c"), "-lz", "-lpthread"], check=True)

    def run(mode, data):
        path = os.path.join(tmp, "in")
        open(path, "wb").write(data)
        p = subprocess.run([exe, mode, path], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        assert p.returncode == 0, (mode, data[:16].hex(), p.returncode, p.stderr[-2000:])
        return int(p.stdout)

    # The review's 16-byte TIFF: IFD at 0xFFFFFFFE, so ifd + 2 wrapped to 0.
    for order in ("II", "MM"):
        for ifd in (0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFF8, 15, 16, 17):
            assert run("raw", tiff(order, ifd, b"\0" * 8)) == 0, (order, hex(ifd))
    # An IFD that claims more entries than the file holds stops at its end.
    assert run("raw", tiff("II", 8, struct.pack("<H", 0xFFFF) + b"\0" * 10)) == 0
    # A real DNG-looking directory is still RAW: one entry, tag 0xC612.
    assert run("raw", tiff("II", 8, struct.pack("<HHHI", 1, 0xC612, 1, 4) + b"\0" * 4)) == 1
    assert run("raw", tiff("MM", 8, struct.pack(">HHHI", 1, 0xC612, 1, 4) + b"\0" * 4)) == 1
    # ZIP: the central directory near 2^32 (cd + 46 wrapped), then the member's
    # local header there (local + 30 wrapped).
    for off in (0xFFFFFFFF, 0xFFFFFFF0, 0xFFFFFFD2, 0xFFFFFF00):
        assert run("cbz", comic(cd_off=off)) == 0, hex(off)
        assert run("cbz", comic(local_off=off)) == 0, hex(off)
    # A well-formed comic still finds its page (stored, so no decoder needed).
    page = b"\x89PNG\r\n\x1a\nnot really a picture"
    body = localhdr(b"pad.txt", b"x" * 40)
    loc = len(body)
    body += localhdr(b"a.png", page)
    cd = central(b"a.png", loc, 0, len(page))
    assert run("cbz", body + cd + eocd(1, len(cd), len(body))) == 1
    print("media_tool: TIFF and ZIP offsets near 2^32 refused, good files still read")
