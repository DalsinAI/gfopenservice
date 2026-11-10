#!/usr/bin/env python3
"""Small Amiga test files for ACMedia, written from the formats' own
definitions (nothing copied): CDXL (CDTV/CD32 video), ILBM (a picture), 8SVX
(a sound) and a ProTracker module. Deterministic, so the checksums of their
decoded frames and samples can be kept.

    python3 gen_amiga.py OUTDIR
MIT, Copyright (c) 2026 AmiCradle Team.
"""
import os
import struct
import sys


def chunk(cid, data):
    pad = b"\0" if len(data) & 1 else b""
    return cid + struct.pack(">I", len(data)) + data + pad


def planar(w, h, planes, pix):
    """pix(x, y) -> colour index; rows of planes interleaved per row (ILBM)
    or whole planes one after another (CDXL)."""
    rowbytes = ((w + 15) // 16) * 2
    rows = []
    for y in range(h):
        for p in range(planes):
            row = bytearray(rowbytes)
            for x in range(w):
                if pix(x, y) >> p & 1:
                    row[x >> 3] |= 0x80 >> (x & 7)
            rows.append(bytes(row))
    return rows, rowbytes


def cdxl(path, frames=8, w=160, h=96, planes=8):
    pal = b"".join(struct.pack(">H", ((i >> 4) << 8) | ((i & 15) << 4) | ((255 - i) >> 4)) for i in range(256))
    rate = 11025
    fps = 12
    alen = rate // fps
    out = bytearray()
    prev = 0
    for f in range(frames):
        rows, rb = planar(w, h, planes, lambda x, y: (x + y + 4 * f) & 255)
        # CDXL holds the planes whole: plane 0's rows, then plane 1's...
        img = b"".join(rows[y * planes + p] for p in range(planes) for y in range(h))
        aud = bytes(((i * (4 + f)) & 0xFF) - 128 & 0xFF for i in range(alen))
        size = 32 + len(pal) + len(img) + len(aud)
        hdr = bytearray(32)
        hdr[0] = 1                       # type: OCS colours
        hdr[1] = 0                       # bit planar, RGB, mono
        struct.pack_into(">III", hdr, 2, size, prev, f + 1)
        struct.pack_into(">HH", hdr, 14, w, h)
        hdr[18] = 0
        hdr[19] = planes
        struct.pack_into(">HHH", hdr, 20, len(pal), alen, rate)
        hdr[26] = fps
        out += hdr + pal + img + aud
        prev = size
    open(path, "wb").write(out)


def ilbm(path, w=96, h=64, planes=5):
    bmhd = struct.pack(">HHhhBBBBHBBhh", w, h, 0, 0, planes, 0, 0, 0, 0, 10, 11, w, h)
    cmap = b"".join(bytes(((i * 8) & 255, (255 - i * 8) & 255, (i * 40) & 255)) for i in range(1 << planes))
    rows, rb = planar(w, h, planes, lambda x, y: ((x // 6) + (y // 4)) % (1 << planes))
    body = b"".join(rows)
    form = b"ILBM" + chunk(b"BMHD", bmhd) + chunk(b"CMAP", cmap) + chunk(b"BODY", body)
    open(path, "wb").write(chunk(b"FORM", form))


def svx(path, n=8000, rate=8000):
    vhdr = struct.pack(">IIIHBBI", n, 0, 32, rate, 1, 0, 0x10000)
    body = bytes((int(100 * (((i // 16) & 1) * 2 - 1)) & 0xFF) for i in range(n))
    form = b"8SVX" + chunk(b"VHDR", vhdr) + chunk(b"NAME", b"acm") + chunk(b"BODY", body)
    open(path, "wb").write(chunk(b"FORM", form))


def mod(path):
    title = b"acmedia test".ljust(20, b"\0")
    slen = 32                                    # words
    samples = struct.pack(">22sHBBHH", b"square", slen, 0, 64, 0, slen)
    samples += b"".join(struct.pack(">22sHBBHH", b"", 0, 0, 0, 0, 1) for _ in range(30))
    order = bytes([0]) + bytes(127)
    head = title + samples + bytes([1, 127]) + order + b"M.K."
    pat = bytearray()
    periods = [428, 381, 339, 320]
    for row in range(64):
        for ch in range(4):
            if row % 4 == 0 and ch == 0:
                p = periods[(row // 4) % 4]
                pat += bytes([0x00 | (p >> 8), p & 255, 0x10, 0x00])   # sample 1
            else:
                pat += bytes(4)
    data = bytes([0x40] * slen + [0xC0] * slen)
    open(path, "wb").write(head + pat + data)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    cdxl(os.path.join(out, "cdxl-stripes.cdxl"))
    ilbm(os.path.join(out, "ilbm-bars.iff"))
    svx(os.path.join(out, "8svx-square.8svx"))
    mod(os.path.join(out, "mod-scale.mod"))


if __name__ == "__main__":
    main()
