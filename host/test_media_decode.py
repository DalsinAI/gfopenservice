"""Checks media_decode.c against files made by avifenc and heif-enc.
MIT, Copyright (c) 2026 AmiCradle Team."""
import os
import struct
import subprocess
import tempfile
import media_decode as md

PROBE, DECODE = 1, 2
AVIF, HEIC = 0x41564946, 0x48454943


def make(tmp):
    """A 640x480 gradient with alpha, as PNG, AVIF (lossless) and HEIC."""
    w, h = 640, 480
    rows = b"".join(b"\0" + b"".join(bytes((x * 255 // w, y * 255 // h, 128, 255 - x * 255 // w)) for x in range(w))
                    for y in range(h))
    import zlib
    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b"")
    p = os.path.join(tmp, "g.png")
    open(p, "wb").write(png)
    subprocess.run(["avifenc", "--lossless", p, os.path.join(tmp, "g.avif")], check=True, capture_output=True)
    subprocess.run(["heif-enc", "-q", "90", p, "-o", os.path.join(tmp, "g.heic")], check=True, capture_output=True)
    return rows, w, h


def call(op, data, maxw=0, maxh=0, room=0, frame=0):
    return md.call(op, frame, [maxw, maxh, 0, 0], 2, [data, None, None, None], [0, room, 0, 0])


with tempfile.TemporaryDirectory() as tmp:
    rows, w, h = make(tmp)
    for name, fmt, exact in (("g.avif", AVIF, True), ("g.heic", HEIC, False)):
        data = open(os.path.join(tmp, name), "rb").read()
        st, pw, ph, out = call(PROBE, data, room=24)
        kind, f, flags, frames, ow, oh = struct.unpack(">6I", out[1])
        assert (st, pw, ph, kind, f, frames, ow, oh) == (0, w, h, 1, fmt, 1, w, h), (name, st, pw, ph, f, ow, oh)
        assert flags & 1, name                                   # alpha
        st, dw, dh, out = call(DECODE, data, room=w * h * 4)
        assert (st, dw, dh) == (0, w, h) and len(out[1]) == w * h * 4, (name, st, dw, dh)
        pix = out[1]
        # ARGB at (x, y) against the source's RGBA
        worst = 0
        for (x, y) in ((0, 0), (320, 240), (639, 479), (100, 400)):
            src = rows[y * (w * 4 + 1) + 1 + x * 4: y * (w * 4 + 1) + 1 + x * 4 + 4]
            got = pix[(y * w + x) * 4:(y * w + x) * 4 + 4]
            worst = max(worst, max(abs(got[0] - src[3]), abs(got[1] - src[0]), abs(got[2] - src[1]), abs(got[3] - src[2])))
        assert worst <= (0 if exact else 12), (name, worst)
        st, pw, ph, out = call(PROBE, data, 200, 200, room=24)
        assert struct.unpack(">6I", out[1])[4:] == (200, 150), name
        st, dw, dh, out = call(DECODE, data, 200, 200, room=200 * 150 * 4)
        assert (st, dw, dh, len(out[1])) == (0, 200, 150, 200 * 150 * 4), (name, st, dw, dh)
        assert call(DECODE, data, 200, 200, room=100)[0] == -4, name
        print(f"{name}: probe {pw}x{ph} alpha, decode exact={exact} worst={worst}, scaled 200x150")
    assert call(PROBE, b"not a picture at all", room=24)[0] == -2
    assert call(DECODE, open(os.path.join(tmp, "g.avif"), "rb").read()[:300], room=w * h * 4)[0] == -2
    print("bad files refused")
