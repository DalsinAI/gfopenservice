"""Checks media.decode/1's and doc.render/1's service-rung calls
(docs/MEDIA_DECODE.md, "Files sent once", "WebM frames by packet" and
"The Amiga's own decoders"):

- a file kept with KEEP gives, named by its handle, what it gives sent
  whole (a picture, a sound, a video); DROP and RELEASE let it go; a handle
  never names another file; the oldest idle file goes when a seventeenth
  comes; a host that has let a file go says GONE (-7); doc.render/1 keeps
  files the same way;
- VINDEX counts a WebM's packets, and VFRAME in MD_PIX_WEBM gives each, in
  order and jumping about, as libvpx's own pictures (decoded by ffmpeg
  through libvpx) drawn with webm.datatype's integer dither: byte for byte;
- NDECODE, when built with the Amiga's own decoders (host/build_native.sh),
  gives PNG, TGA, GIF and lossless WebP pictures as Pillow reads them, and
  refuses what it can't decode. (gfopendatatypes' tests/test-service-rung.sh
  checks NDECODE against the 68k, under qemu-m68k, hash for hash.)
MIT, Copyright (c) 2026 AmiCradle Team."""
import io
import os
import struct
import subprocess
import tempfile
import media_decode as md

PROBE, DECODE, VOPEN, VFRAME, VCLOSE, VINDEX, NDECODE, RELEASE = 1, 2, 3, 4, 5, 32, 33, 34
KEEP, DROP, GONE = 2, 4, -7
PIX_WEBM = 3
failures = 0


def check(ok, what):
    global failures
    if not ok:
        failures += 1
        print("FAIL", what)


def keep_call(op, arg, extra, data, length, keep=False, handle=0, drop=False):
    """op on data (or on the kept file handle when data is None); buf1 out of
    length bytes, buf3 the handle KEEP writes. (status, result, aux, out, handle)."""
    e3 = (extra[3] if len(extra) > 3 else 0) | (KEEP if keep else 0) | (DROP if drop else 0) | (handle << 8)
    ex = list(extra[:3]) + [e3]
    st, r, a, w = md.call(op, arg, ex, 2 | 8, [data if data is not None else b"", None, None, None],
                          [0, length, 0, 4])
    h = struct.unpack(">I", w[3])[0] if len(w.get(3, b"")) == 4 else 0
    return st, r, a, w.get(1, b""), h


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *args], check=True)


def webm_dither_ref(yuv, w, h):
    """webm/webm_dither.h on an I420 picture."""
    t_y = [(298 * (i - 16)) >> 2 for i in range(256)]
    t_rv = [(409 * (i - 128)) >> 2 for i in range(256)]
    t_gu = [(-100 * (i - 128)) >> 2 for i in range(256)]
    t_gv = [(-208 * (i - 128)) >> 2 for i in range(256)]
    t_bu = [(516 * (i - 128)) >> 2 for i in range(256)]
    bayer = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
    cw, ch = (w + 1) // 2, (h + 1) // 2
    Y, U, V = yuv[:w * h], yuv[w * h:w * h + cw * ch], yuv[w * h + cw * ch:]
    out = bytearray(w * h)

    def c8(v):
        return 0 if v < 0 else 255 if v > 255 else v
    for y in range(h):
        for x in range(w):
            luma = t_y[Y[y * w + x]]
            u, v = U[(y >> 1) * cw + (x >> 1)], V[(y >> 1) * cw + (x >> 1)]
            r = c8((luma + t_rv[v] + 32) >> 6)
            g = c8((luma + t_gu[u] + t_gv[v] + 32) >> 6)
            b = c8((luma + t_bu[u] + 32) >> 6)
            d = bayer[y & 3][x & 3] * 16
            out[y * w + x] = ((r * 5 + d) >> 8) * 36 + ((g * 5 + d) >> 8) * 6 + ((b * 5 + d) >> 8)
    return bytes(out)


with tempfile.TemporaryDirectory() as tmp:
    # ---- files sent once ---------------------------------------------------
    png = os.path.join(tmp, "p.png")
    ffmpeg("-f", "lavfi", "-i", "testsrc2=s=96x64", "-frames:v", "1", png)
    pdata = open(png, "rb").read()
    st, w, h, whole, _ = keep_call(DECODE, 0, [0, 0, 0, 0], pdata, 96 * 64 * 4)
    check(st == 0 and (w, h) == (96, 64), f"DECODE of the PNG sent whole: {st} {w}x{h}")
    st, w, h, info, handle = keep_call(PROBE, 0, [0, 0, 0, 0], pdata, 24, keep=True)
    check(st == 0 and handle, f"PROBE with KEEP: status {st}, handle {handle:#x}")
    st, w, h, kept, h2 = keep_call(DECODE, 0, [0, 0, 0, 0], None, 96 * 64 * 4, handle=handle)
    check(st == 0 and kept == whole, f"DECODE of the kept PNG: {st}, same pixels {kept == whole}")
    st, *_ = keep_call(DECODE, 0, [0, 0, 0, 0], None, 96 * 64 * 4, handle=handle, drop=True)
    check(st == 0, f"DECODE with DROP: {st}")
    st, *_ = keep_call(DECODE, 0, [0, 0, 0, 0], None, 96 * 64 * 4, handle=handle)
    check(st == GONE, f"after DROP: {st}, not GONE")
    st, *_ = keep_call(PROBE, 0, [0, 0, 0, 0], None, 24, handle=0x123457)
    check(st == GONE, f"a handle never given: {st}")
    st, _, _, _, handle = keep_call(PROBE, 0, [0, 0, 0, 0], pdata, 24, keep=True)
    st2, *_ = md.call(RELEASE, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
    st3, *_ = keep_call(PROBE, 0, [0, 0, 0, 0], None, 24, handle=handle)
    check(st2 == 0 and st3 == GONE, f"RELEASE {st2}, then {st3}")
    st, *_ = md.call(RELEASE, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
    check(st == GONE, f"RELEASE twice: {st}")
    # Seventeen kept: the first goes, the others stay, and its slot's next
    # file does not answer to the old handle.
    handles = []
    for i in range(17):
        st, _, _, _, hd = keep_call(PROBE, 0, [0, 0, 0, 0], pdata, 24, keep=True)
        handles.append(hd)
    gone = [keep_call(PROBE, 0, [0, 0, 0, 0], None, 24, handle=hd)[0] for hd in handles]
    check(gone[0] == GONE and all(g == 0 for g in gone[1:]),
          f"17 kept: {gone.count(GONE)} gone, the first {gone[0]}")
    check(len(set(handles)) == 17, "handles given twice")
    for hd in handles:
        md.call(RELEASE, hd, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
    # A sound: PROBE kept, DECODE in pieces by handle as with the file.
    wav = os.path.join(tmp, "s.flac")
    ffmpeg("-f", "lavfi", "-i", "sine=f=440:d=3:r=22050", "-ac", "1", wav)
    sdata = open(wav, "rb").read()
    st, frames, rate, info, handle = keep_call(PROBE, 0, [1, 22050, 0, 0], sdata, 24, keep=True)
    check(st == 0 and handle and frames > 60000, f"sound PROBE with KEEP: {st} {frames} {handle:#x}")
    same = True
    for at in (0, 22050, 50000):
        a = keep_call(DECODE, at, [1, 22050, 0, 0], sdata, 8000)
        b = keep_call(DECODE, at, [1, 22050, 0, 0], None, 8000, handle=handle)
        same = same and a[0] == b[0] == 0 and a[3] == b[3] and a[1] == b[1]
    check(same, "the kept sound's pieces are the file's")
    md.call(RELEASE, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
    print(f"files sent once: kept, named, dropped, released, pushed out: {'ok' if not failures else 'FAILED'}")

    # ---- doc.render/1 keeps files the same way ------------------------------
    try:
        import doc_render as dr
        st, *_ = dr.call(RELEASE, 0x55, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
        st2, *_ = dr.call(1, 0, [800, 0, 0, 0x99 << 8], 2, [b"", None, None, None], [0, 24, 0, 0])
        check(st == GONE and st2 == GONE, f"doc.render/1: RELEASE of nothing {st}, a handle never given {st2}")
        print("doc.render/1: unknown kept files answer GONE")
    except OSError:
        print("doc.render/1 not built: skipped")

    # ---- WebM frames by packet ---------------------------------------------
    for name, args in (("vp8.webm", ["-c:v", "libvpx", "-b:v", "300k", "-auto-alt-ref", "0", "-g", "12"]),
                       ("vp9.webm", ["-c:v", "libvpx-vp9", "-b:v", "300k", "-g", "12"])):
        path = os.path.join(tmp, name)
        ffmpeg("-f", "lavfi", "-i", "testsrc2=s=96x64:r=10:d=3", *args, path)
        data = open(path, "rb").read()
        st, handle, fps, w = md.call(VOPEN, 0, [16, 16, 0, 0], 2, [data, None, None, None], [0, 24, 0, 0])
        st2, count, wh, _ = md.call(VINDEX, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
        check(st == 0 and st2 == 0 and wh == (96 << 16 | 64), f"{name}: VOPEN {st}, VINDEX {st2}, size {wh:#x}")
        # libvpx's pictures through ffmpeg, as the 68k's libvpx makes them
        dec = "libvpx" if name.startswith("vp8") else "libvpx-vp9"
        raw = subprocess.run(["ffmpeg", "-v", "error", "-c:v", dec, "-i", path, "-fps_mode", "passthrough",
                              "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"], capture_output=True, check=True).stdout
        n = 96 * 64 * 3 // 2
        ref = [webm_dither_ref(raw[i:i + n], 96, 64) for i in range(0, len(raw) - n + 1, n)]
        check(count == len(ref), f"{name}: VINDEX counts {count} packets, libvpx shows {len(ref)} pictures")
        order = list(range(count)) + [count - 1, 0, 5, 5, 6, 3, 17, 2, 29, count + 9]
        bad = 0
        for f in order:
            st, got, _, out = md.call(VFRAME, handle, [f, PIX_WEBM, 96, 64], 2, [None] * 4, [0, 96 * 64, 0, 0])
            want = min(f, count - 1)
            if st != 0 or got != want or out.get(1) != ref[want]:
                bad += 1
        check(bad == 0, f"{name}: {bad} of {len(order)} frames differ from libvpx's")
        st, *_ = md.call(VFRAME, handle, [0, PIX_WEBM, 96, 64], 2, [None] * 4, [0, 100, 0, 0])
        check(st == -4, f"{name}: a buffer too small gives {st}")
        md.call(VCLOSE, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
        print(f"{name}: {count} packets, {len(order)} frames in and out of order as libvpx's, "
              f"in webm.datatype's colours: {'ok' if not bad else 'FAILED'}")
    mp4 = os.path.join(tmp, "h.mp4")
    ffmpeg("-f", "lavfi", "-i", "testsrc2=s=64x48:r=10:d=1", "-c:v", "libx264", mp4)
    st, handle, *_ = md.call(VOPEN, 0, [16, 16, 0, 0], 2, [open(mp4, "rb").read(), None, None, None], [0, 24, 0, 0])
    st2, *_ = md.call(VINDEX, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
    check(st == 0 and st2 == -2, f"VINDEX of H.264: {st2}, not refused")
    md.call(VCLOSE, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)

    # ---- the Amiga's own decoders --------------------------------------------
    st, *_ = md.call(NDECODE, 0, [0x504e4720, 0, 0, 0], 6, [pdata, None, None, None], [0, 96 * 64 * 4, 16, 0])
    if st == -2:
        print("NDECODE: not built with the Amiga's decoders (host/build_native.sh): skipped")
    else:
        try:
            from PIL import Image
        except ImportError:
            Image = None
        if Image:
            def argb(im):
                rgba = im.convert("RGBA").tobytes()
                out = bytearray(len(rgba))
                out[0::4], out[1::4], out[2::4], out[3::4] = rgba[3::4], rgba[0::4], rgba[1::4], rgba[2::4]
                return bytes(out)
            src = Image.open(io.BytesIO(pdata))
            for kind, fmt, opts, fourcc in (("PNG", "PNG", {}, 0x504e4720), ("TGA", "TGA", {}, 0x54474120),
                                            ("TGA RLE", "TGA", {"compression": "tga_rle"}, 0x54474120),
                                            ("WebP lossless", "WEBP", {"lossless": True}, 0x57454250)):
                for mode in ("RGB", "RGBA"):
                    b = io.BytesIO()
                    im = src.convert(mode)
                    if mode == "RGBA":
                        im.putalpha(Image.linear_gradient("L").resize(im.size))
                    im.save(b, fmt, **opts)
                    st, w, h, out = md.call(NDECODE, 0, [fourcc, 0, 0, 0], 6, [b.getvalue(), None, None, None],
                                            [0, 96 * 64 * 4, 16, 0])
                    info = struct.unpack(">4I", out.get(2, bytes(16)))
                    check(st == 0 and out.get(1) == argb(im) and info[:3] == (96, 64, int(mode == "RGBA")),
                          f"NDECODE {kind} {mode}: {st}, pixels as Pillow's {out.get(1) == argb(im)}, info {info}")
            b = io.BytesIO()
            pal = src.convert("P", palette=Image.ADAPTIVE, colors=64)
            pal.save(b, "GIF")
            st, w, h, out = md.call(NDECODE, 0, [0x47494620, 1, 0, 0], 6, [b.getvalue(), None, None, None],
                                    [0, 96 * 64, 16, 0])
            check(st == 0 and out.get(1) == Image.open(io.BytesIO(b.getvalue())).tobytes(),
                  f"NDECODE GIF indexes: {st}")
            st, w, h, out = md.call(NDECODE, 0, [0x47494620, 0, 0, 0], 6, [b.getvalue(), None, None, None],
                                    [0, 96 * 64 * 4, 16, 0])
            check(st == 0 and out.get(1) == argb(Image.open(io.BytesIO(b.getvalue()))), f"NDECODE GIF ARGB: {st}")
        st, *_ = md.call(NDECODE, 0, [0x504e4720, 0, 0, 0], 6, [pdata[:200], None, None, None], [0, 96 * 64 * 4, 16, 0])
        check(st == -2, f"NDECODE of a cut PNG: {st}")
        st, w, h, _ = md.call(NDECODE, 0, [0x504e4720, 0, 0, 0], 6, [pdata, None, None, None], [0, 100, 16, 0])
        check(st == -4 and (w, h) == (96, 64), f"NDECODE into too small a buffer: {st} {w}x{h}")
        st, *_ = md.call(NDECODE, 0, [0x4a504547, 0, 0, 0], 6, [pdata, None, None, None], [0, 96 * 64 * 4, 16, 0])
        check(st == -2, f"NDECODE with a decoder it has not: {st}")
        st, _, _, out, hd = keep_call(PROBE, 0, [0, 0, 0, 0], pdata, 24, keep=True)
        a = md.call(NDECODE, 0, [0x504e4720, 0, 0, 0], 6, [pdata, None, None, None], [0, 96 * 64 * 4, 16, 0])
        b = md.call(NDECODE, 0, [0x504e4720, 0, 0, hd << 8 | DROP], 6, [b"", None, None, None], [0, 96 * 64 * 4, 16, 0])
        check(a[0] == b[0] == 0 and a[3][1] == b[3][1], "NDECODE of a kept file")
        print(f"NDECODE: PNG, TGA, GIF and WebP as Pillow reads them: {'ok' if not failures else 'FAILED'}")

print("FAILED" if failures else "all ok")
raise SystemExit(1 if failures else 0)
