"""Checks media_decode's picture chain past AVIF and HEIC: FFmpeg's still
readers (JPEG, PNG, GIF, WebP, JPEG XL, QOI, PSD...), camera RAW through
LibRaw and anything else through ImageMagick, with the extension hint.
MIT, Copyright (c) 2026 AmigaChrome Team."""
import os
import struct
import subprocess
import tempfile
import time
import zlib

PROBE, DECODE = 1, 2


def fourcc(s):
    return struct.unpack(">I", s.encode())[0]


def gradient(tmp, w=320, h=240):
    rows = b"".join(b"\0" + b"".join(bytes((x * 255 // w, y * 255 // h, 128, 255)) for x in range(w)) for y in range(h))
    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b"")
    open(os.path.join(tmp, "g.png"), "wb").write(png)
    return rows, w, h


def ff(tmp, out, *args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", os.path.join(tmp, "g.png"), *args, os.path.join(tmp, out)],
                   check=True)


def im(tmp, out, *args):
    subprocess.run(["convert", os.path.join(tmp, "g.png"), *args, os.path.join(tmp, out)], check=True)


def call(op, data, hint="", maxw=0, maxh=0, room=0):
    h = fourcc((hint.upper() + "    ")[:4]) if hint else 0
    return md.call(op, 0, [maxw, maxh, h, 0], 2, [data, None, None, None], [0, room, 0, 0])


def worst(rows, w, pix):
    bad = 0
    for (x, y) in ((0, 0), (160, 120), (319, 239), (50, 200)):
        src = rows[y * (w * 4 + 1) + 1 + x * 4:][:4]
        got = pix[(y * w + x) * 4:][:4]
        bad = max(bad, abs(got[1] - src[0]), abs(got[2] - src[1]), abs(got[3] - src[2]), abs(got[0] - 255))
    return bad


with tempfile.TemporaryDirectory() as tmp:
    os.environ["OPENSERVICE_CACHE"] = os.path.join(tmp, "cache")
    import media_decode as md
    rows, w, h = gradient(tmp)
    cases = [  # file, how to make it, hint, format, worst allowed
        ("g.jpg", lambda: ff(tmp, "g.jpg", "-q:v", "2", "-pix_fmt", "yuvj444p"), "", "JPEG", 12),
        ("g.png", lambda: None, "", "PNG ", 0),
        ("g.gif", lambda: ff(tmp, "g.gif"), "", "GIF ", 64),
        ("g.webp", lambda: ff(tmp, "g.webp", "-c:v", "libwebp", "-lossless", "1"), "", "WEBP", 0),
        ("g.jxl", lambda: ff(tmp, "g.jxl", "-c:v", "libjxl", "-distance", "0"), "", "JXL ", 0),
        ("g.qoi", lambda: ff(tmp, "g.qoi"), "", "QOI ", 0),
        ("g.exr", lambda: ff(tmp, "g.exr", "-pix_fmt", "gbrpf32le"), "", "EXR ", 3),
        ("g.psd", lambda: im(tmp, "g.psd"), "", None, 0),  # FFmpeg or ImageMagick
        ("g.tga", lambda: ff(tmp, "g.tga"), "tga", "TGA ", 0),  # ImageMagick 6 writes TGA upside down
        ("g.pcx", lambda: im(tmp, "g.pcx"), "", None, 0),  # FFmpeg refuses this one; ImageMagick answers
        ("g.xpm", lambda: im(tmp, "g.xpm", "-colors", "64"), "xpm", None, 40),
    ]
    for name, make, hint, fmt, allowed in cases:
        try:
            make()
        except Exception as e:  # an encoder this FFmpeg or ImageMagick lacks
            print(f"{name}: skipped ({e.__class__.__name__})")
            continue
        data = open(os.path.join(tmp, name), "rb").read()
        st, pw, ph, out = call(PROBE, data, hint, room=24)
        assert st == 0, (name, st)
        kind, f, flags, frames, ow, oh = struct.unpack(">6I", out[1])
        assert (kind, pw, ph, ow, oh) == (1, w, h, w, h), (name, kind, pw, ph)
        if fmt:
            assert f == fourcc(fmt), (name, struct.pack(">I", f))
        t = time.perf_counter()
        st, dw, dh, out = call(DECODE, data, hint, room=w * h * 4)
        ms = (time.perf_counter() - t) * 1000
        assert (st, dw, dh, len(out[1])) == (0, w, h, w * h * 4), (name, st, dw, dh)
        bad = worst(rows, w, out[1])
        assert bad <= allowed, (name, bad)
        st, dw, dh, out = call(DECODE, data, hint, 100, 100, room=100 * 75 * 4)
        assert (st, dw, dh) == (0, 100, 75), (name, st, dw, dh)
        print(f"{name}: {struct.pack('>I', f).decode()} full size {dw and w}x{h}, worst {bad}, decode {ms:.1f} ms")

    # JSC's case: the formats a web page carries, one request each, full size.
    for name in ("g.jpg", "g.png", "g.gif", "g.webp"):
        if not os.path.exists(os.path.join(tmp, name)):   # its encoder is missing here
            continue
        data = open(os.path.join(tmp, name), "rb").read()
        t = time.perf_counter()
        for _ in range(20):
            assert call(DECODE, data, room=w * h * 4)[0] == 0
        print(f"{name}: {(time.perf_counter() - t) * 50:.2f} ms per full-size decode on the host")

    # Pictures inside ZIPs: OpenRaster and Krita (their flattened picture), a comic's first page.
    import zipfile
    png = open(os.path.join(tmp, "g.png"), "rb").read()
    for name, mime, fmt in (("g.ora", "image/openraster", "ORA "), ("g.kra", "application/x-krita", "KRA ")):
        p = os.path.join(tmp, name)
        with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr(zipfile.ZipInfo("mimetype"), mime)
            z.writestr("stack.xml", "<image/>")
            z.writestr("mergedimage.png", png)
        data = open(p, "rb").read()
        st, pw, ph, out = call(PROBE, data, room=24)
        assert st == 0 and struct.unpack(">6I", out[1])[1] == fourcc(fmt), (name, st)
        st, dw, dh, out = call(DECODE, data, room=w * h * 4)
        assert st == 0 and worst(rows, w, out[1]) == 0, (name, st)
        print(f"{name}: {fmt} {dw}x{dh} from mergedimage.png")
    p = os.path.join(tmp, "g.cbz")
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("page02.jpg", b"not this one")
        z.writestr("page01.png", png)
    data = open(p, "rb").read()
    st, pw, ph, out = call(PROBE, data, "cbz", room=24)
    assert st == 0 and struct.unpack(">6I", out[1])[1] == fourcc("CBZ "), st
    assert call(PROBE, data, room=24)[0] == -2                          # a ZIP not named a comic
    print(f"g.cbz: CBZ {pw}x{ph}, the first page by name")

    # Camera RAW: a linear DNG, if tifffile can write one.
    try:
        import numpy as np
        import tifffile
        img = np.zeros((h, w, 3), np.uint16)
        for y in range(h):
            for x in range(w):
                img[y, x] = (x * 65535 // w, y * 65535 // h, 32896)
        p = os.path.join(tmp, "g.dng")
        tifffile.imwrite(p, img, photometric=34892, extratags=[(50706, "B", 4, b"\1\4\0\0", True),
                                                              (50708, "s", 0, "GFOpenService test", True)])
        data = open(p, "rb").read()
        st, pw, ph, out = call(PROBE, data, room=24)
        assert st == 0 and struct.unpack(">6I", out[1])[1] == fourcc("RAW "), st
        st, dw, dh, out = call(DECODE, data, room=pw * ph * 4)
        assert st == 0, st
        print(f"g.dng: RAW {pw}x{ph} through LibRaw")
    except ImportError:
        print("g.dng: skipped (no tifffile)")

    assert call(PROBE, b"not a picture at all" * 10, room=24)[0] == -2
    assert call(PROBE, os.urandom(4000), "cr2", room=24)[0] == -2
    print("bad files refused")
