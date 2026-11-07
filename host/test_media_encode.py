"""Checks media.encode/1 (media_encode.c, heif_mux.c) against outside
readers: HEIC pictures read by libheif (the container always; the pixels
too when its HEVC decoder plugin is there) and by FFmpeg's HEVC decoder from
the items' own bytes; M4A, AAC and WMA sounds and HEVC and WMV films read
back by ffprobe and ffmpeg (codec, container, rate, size, frames, a 440 Hz
tone, PSNR against the pixels sent). Then the requests it must refuse.
MIT, Copyright (c) 2026 AmiCradle Team."""
import ctypes
import ctypes.util
import json
import math
import os
import struct
import subprocess
import tempfile
import time

import media_encode as me


def ff(*args, data=None):
    return subprocess.run(["ffmpeg", "-nostdin", "-v", "error", *args], input=data, capture_output=True,
                          check=True).stdout


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_format", "-show_streams", "-of", "json",
                          path], check=True, capture_output=True).stdout
    j = json.loads(out)
    return j["format"], {s["codec_type"]: s for s in j["streams"]}


def psnr(a, b, step=1, offset=0):
    n = len(range(offset, min(len(a), len(b)), step))
    se = sum((a[i] - b[i]) ** 2 for i in range(offset, min(len(a), len(b)), step)) / n
    return 99.0 if se == 0 else 10 * math.log10(255 * 255 / se)


def crossings(pcm, rate):
    mid = pcm[rate // 4: rate // 4 + rate // 2]        # half a second from the middle
    return sum(1 for x, y in zip(mid, mid[1:]) if x < 0 <= y) * 2


# ---- HEIF: the boxes, as a reader sees them -----------------------------------

def boxes(d, at=0, end=None):
    end = len(d) if end is None else end
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", d[at:at + 8])
        assert size >= 8 and at + size <= end, (kind, size)
        yield kind.decode(), at + 8, at + size
        at += size


def heif_parts(d):
    """The items' properties and bytes: a small reader of our own layout."""
    top = {k: (s, e) for k, s, e in boxes(d)}
    assert d[top["ftyp"][0]:top["ftyp"][0] + 4] == b"heic" and b"mif1" in d[top["ftyp"][0]:top["ftyp"][1]]
    ms, mend = top["meta"]
    meta = {k: (s, e) for k, s, e in boxes(d, ms + 4, mend)}
    s, e = meta["hdlr"]
    assert d[s + 8:s + 12] == b"pict"
    s, e = meta["iloc"]
    assert d[s] == 0 and d[s + 4] == 0x44
    count = struct.unpack(">H", d[s + 6:s + 8])[0]
    items, at = {}, s + 8
    for _ in range(count):
        iid, _, extents, off, length = struct.unpack(">HHHII", d[at:at + 14])
        assert extents == 1
        items[iid] = d[off:off + length]
        assert top["mdat"][0] <= off and off + length <= top["mdat"][1]
        at += 14
    s, e = meta["iprp"]
    ip = {k: (s2, e2) for k, s2, e2 in boxes(d, s, e)}
    props = [(k, d[s2:e2]) for k, s2, e2 in boxes(d, *ip["ipco"])]
    s, e = ip["ipma"]
    n = struct.unpack(">I", d[s + 4:s + 8])[0]
    assoc, at = {}, s + 8
    for _ in range(n):
        iid, k = struct.unpack(">HB", d[at:at + 3])
        assoc[iid] = {props[(x & 0x7f) - 1][0]: props[(x & 0x7f) - 1][1] for x in d[at + 3:at + 3 + k]}
        at += 3 + k
    return items, assoc, "iref" in meta


def annexb(hvcc, data):
    """An item as a raw HEVC stream: hvcC's parameter sets, then its NAL units."""
    out, at = b"", 23
    for _ in range(hvcc[22]):
        n = struct.unpack(">H", hvcc[at + 1:at + 3])[0]
        at += 3
        for _ in range(n):
            ln = struct.unpack(">H", hvcc[at:at + 2])[0]
            out += b"\0\0\0\1" + hvcc[at + 2:at + 2 + ln]
            at += 2 + ln
    at = 0
    while at < len(data):
        ln = struct.unpack(">I", data[at:at + 4])[0]
        out += b"\0\0\0\1" + data[at + 4:at + 4 + ln]
        at += 4 + ln
    return out


class Heif:
    """libheif (the runtime library, no headers needed): reads the file; decodes
    when its HEVC decoder plugin is there."""
    def __init__(self):
        name = ctypes.util.find_library("heif")
        self.lib = ctypes.CDLL(name) if name else None
        if not self.lib:
            return
        L = self.lib
        L.heif_context_alloc.restype = ctypes.c_void_p
        L.heif_context_read_from_memory_without_copy.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                                                                 ctypes.c_void_p]
        L.heif_context_read_from_memory_without_copy.restype = HeifError
        L.heif_context_get_primary_image_handle.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        L.heif_context_get_primary_image_handle.restype = HeifError
        for f in ("heif_image_handle_get_width", "heif_image_handle_get_height", "heif_image_handle_has_alpha_channel"):
            getattr(L, f).argtypes = [ctypes.c_void_p]
            getattr(L, f).restype = ctypes.c_int
        L.heif_have_decoder_for_format.argtypes = [ctypes.c_int]
        L.heif_decode_image.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_int, ctypes.c_int,
                                        ctypes.c_void_p]
        L.heif_decode_image.restype = HeifError
        L.heif_image_get_plane_readonly.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        L.heif_image_get_plane_readonly.restype = ctypes.c_void_p
        L.heif_image_release.argtypes = [ctypes.c_void_p]
        L.heif_image_handle_release.argtypes = [ctypes.c_void_p]
        L.heif_context_free.argtypes = [ctypes.c_void_p]
        self.decodes = bool(L.heif_have_decoder_for_format(1))      # heif_compression_HEVC

    def read(self, data, decode):
        L, ctx, h = self.lib, self.lib.heif_context_alloc(), ctypes.c_void_p()
        try:
            err = L.heif_context_read_from_memory_without_copy(ctx, data, len(data), None)
            assert err.code == 0, err.message
            err = L.heif_context_get_primary_image_handle(ctx, ctypes.byref(h))
            assert err.code == 0, err.message
            w, ht = L.heif_image_handle_get_width(h), L.heif_image_handle_get_height(h)
            alpha = bool(L.heif_image_handle_has_alpha_channel(h))
            rgba = None
            if decode:
                img = ctypes.c_void_p()
                err = L.heif_decode_image(h, ctypes.byref(img), 1, 11, None)   # RGB, interleaved RGBA
                assert err.code == 0, err.message
                stride = ctypes.c_int()
                p = L.heif_image_get_plane_readonly(img, 10, ctypes.byref(stride))   # interleaved
                rgba = b"".join(ctypes.string_at(p + y * stride.value, w * 4) for y in range(ht))
                L.heif_image_release(img)
            L.heif_image_handle_release(h)
            return w, ht, alpha, rgba
        finally:
            L.heif_context_free(ctx)


class HeifError(ctypes.Structure):
    _fields_ = [("code", ctypes.c_int), ("subcode", ctypes.c_int), ("message", ctypes.c_char_p)]


def argb_to_rgba(b):
    out = bytearray(len(b))
    out[0::4], out[1::4], out[2::4], out[3::4] = b[1::4], b[2::4], b[3::4], b[0::4]
    return bytes(out)


fmts = me.formats()
print("formats:", " ".join(f.strip() for f in fmts))
assert set(fmts) == {"HEIC", "M4A ", "AAC ", "WMA ", "HEVC", "WMV "}, fmts
heif = Heif()

with tempfile.TemporaryDirectory() as tmp:
    # ---- pictures ------------------------------------------------------------
    # Colour gradients, and an alpha that ramps across them.
    for w, h in ((320, 240), (321, 241)):
        rgb = ff("-f", "lavfi", "-i", f"gradients=size={w + 1}x{h + 1}:seed=3,format=rgb24,crop={w}:{h}:0:0",
                 "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
        argb = b"".join(bytes((255 * (i % w) // w,)) + rgb[i * 3:i * 3 + 3] for i in range(w * h))
        for pixels, data, alpha in ((me.PIX_ARGB, argb, False), (me.PIX_ARGB, argb, True), (me.PIX_RGB, rgb, False)):
            t = time.perf_counter()
            st, info, heic = me.encode(me.settings("HEIC", w, h, pixels, flags=me.F_ALPHA if alpha else 0), data,
                                       piece=w * 4 * 7 + 3)      # pieces that cut rows anywhere
            took = time.perf_counter() - t
            assert st == 0 and info["bytes"] == len(heic) and (info["width"], info["height"]) == (w, h), (st, info)
            items, assoc, has_iref = heif_parts(heic)
            cw, ch = (w + 1) & ~1, (h + 1) & ~1
            p = assoc[1]
            assert struct.unpack(">II", p["ispe"][4:12]) == (cw, ch)
            assert p["colr"][:4] == b"nclx" and struct.unpack(">HHH", p["colr"][4:10]) == (1, 13, 6) \
                and p["colr"][10] == 0x80
            assert ("clap" in p) == ((w, h) != (cw, ch))
            assert (2 in items) == alpha == has_iref
            # the picture's own bytes through FFmpeg's HEVC decoder, cropped to its size
            got = ff("-f", "hevc", "-i", "-", "-vf", f"scale=in_color_matrix=bt601:in_range=full,format=rgb24,crop={w}:{h}:0:0",
                     "-f", "rawvideo", "-pix_fmt", "rgb24", "-", data=annexb(p["hvcC"], items[1]))
            q = psnr(rgb, got)
            assert q > 32, (w, h, q)
            line = f"HEIC {w}x{h} {'ARGB' if pixels == me.PIX_ARGB else 'RGB'}{' alpha' if alpha else ''}: " \
                   f"{len(heic)} bytes in {took * 1000:.0f} ms, FFmpeg {q:.1f} dB"
            if alpha:
                a = ff("-f", "hevc", "-i", "-", "-vf", f"crop={w}:{h}:0:0", "-f", "rawvideo", "-pix_fmt", "gray",
                       "-", data=annexb(assoc[2]["hvcC"], items[2]))
                assert assoc[2]["auxC"][4:].rstrip(b"\0") == b"urn:mpeg:hevc:2015:auxid:1"
                qa = psnr(argb[0::4], a)
                assert qa > 40, qa
                line += f", alpha {qa:.1f} dB"
            if heif.lib:
                hw, hh, ha, rgba = heif.read(heic, heif.decodes)
                assert (hw, hh, ha) == (w, h, alpha), (hw, hh, ha)
                line += ", libheif reads it"
                if rgba:
                    src = argb_to_rgba(argb)
                    qh = min(psnr(src, rgba, 4, c) for c in range(3))
                    assert qh > 32, qh
                    line += f" and decodes it, {qh:.1f} dB"
                    if alpha:
                        qa = psnr(src, rgba, 4, 3)
                        assert qa > 40, qa
                        line += f", alpha {qa:.1f} dB"
                    else:
                        assert set(rgba[3::4]) == {255}
            open(os.path.join(tmp, "p.heic"), "wb").write(heic)
            print(line)
    if heif.lib and not heif.decodes:
        print("libheif: no HEVC decoder plugin here, so only its reading of the files was checked")

    # ---- sounds ----------------------------------------------------------------
    def tone(rate, channels, s8, seconds=2):
        pcm = ff("-f", "lavfi", "-i", f"sine=frequency=440:sample_rate={rate}:duration={seconds}",
                 "-ac", str(channels), "-f", "s8" if s8 else "s16be", "-")
        return pcm

    for fmt, codec, container in (("M4A ", "aac", "mov"), ("AAC ", "aac", "aac"), ("WMA ", "wmav2", "asf")):
        for rate, channels, s8, coded in ((28000, 2, False, 32000), (22050, 1, True, 22050), (44100, 2, False, 44100)):
            pcm = tone(rate, channels, s8)
            t = time.perf_counter()
            st, info, out = me.encode(me.settings(fmt, rate=rate, channels=channels,
                                                  samples=me.SAMPLE_S8 if s8 else me.SAMPLE_S16), sound=pcm)
            took = time.perf_counter() - t
            frames = len(pcm) // (channels * (1 if s8 else 2))
            assert st == 0 and info["sample_frames"] == frames and info["bytes"] == len(out), (fmt, st, info)
            path = os.path.join(tmp, "s." + fmt.strip().lower())
            open(path, "wb").write(out)
            f, s = probe(path)
            a = s["audio"]
            assert container in f["format_name"] and a["codec_name"] == codec, (fmt, f["format_name"], a["codec_name"])
            assert (int(a["sample_rate"]), a["channels"]) == (coded, channels), (fmt, a["sample_rate"], a["channels"])
            assert abs(float(f["duration"]) - 2) < 0.15, (fmt, f["duration"])
            back = ff("-i", path, "-ac", "1", "-f", "s16le", "-")
            back = struct.unpack("<%dh" % (len(back) // 2), back)
            c = crossings(back, coded)
            assert 430 <= c <= 450, (fmt, rate, c)
            print(f"{fmt.strip()} {rate} Hz x{channels} {'8' if s8 else '16'}-bit: {codec} in {f['format_name']} "
                  f"at {coded} Hz, {len(out)} bytes in {took * 1000:.0f} ms, {c} Hz tone")

    # ---- films -----------------------------------------------------------------
    for fmt, codec, container, acodec in (("HEVC", "hevc", "mp4", "aac"), ("WMV ", "wmv2", "asf", "wmav2")):
        for w, h, pixels, sound in ((160, 120, me.PIX_RGB, True), (161, 121, me.PIX_ARGB, False)):
            frames = ff("-f", "lavfi", "-i", f"gradients=size={w + 1}x{h + 1}:rate=25:duration=2:speed=0.05:seed=5,"
                        f"format=rgb24,crop={w}:{h}:0:0", "-f", "rawvideo",
                        "-pix_fmt", "argb" if pixels == me.PIX_ARGB else "rgb24", "-")
            fb = w * h * (4 if pixels == me.PIX_ARGB else 3)
            snd = tone(22050, 1, True) if sound else b""
            t = time.perf_counter()
            st, info, out = me.encode(me.settings(fmt, w, h, pixels, fps1000=25000, rate=22050 if sound else 0,
                                                  channels=1 if sound else 0, samples=me.SAMPLE_S8),
                                      pictures=frames, sound=snd, piece=fb // 3 + 5)
            took = time.perf_counter() - t
            assert st == 0 and info["frames"] == 50 and info["bytes"] == len(out), (fmt, st, info)
            path = os.path.join(tmp, "f." + container)
            open(path, "wb").write(out)
            f, s = probe(path)
            v = s["video"]
            assert container in f["format_name"] and v["codec_name"] == codec, (fmt, f["format_name"], v["codec_name"])
            assert (v["width"], v["height"]) == ((w + 1) & ~1, (h + 1) & ~1) and int(v["nb_read_frames"]) == 50, v
            if fmt == "HEVC":
                assert v["codec_tag_string"] == "hvc1"
            assert ("audio" in s) == sound and (not sound or s["audio"]["codec_name"] == acodec), s.keys()
            got = ff("-i", path, "-vf", f"format=rgb24,crop={w}:{h}:0:0,select=eq(n\\,10)", "-frames:v", "1", "-f", "rawvideo",
                     "-pix_fmt", "rgb24", "-")
            src = frames[10 * fb:11 * fb]
            if pixels == me.PIX_ARGB:
                src = b"".join(src[i + 1:i + 4] for i in range(0, len(src), 4))
            q = psnr(src, got)
            assert q > 30, (fmt, q)
            line = f"{fmt.strip()} {w}x{h}: {codec} in {f['format_name']}, 50 frames, {q:.1f} dB at frame 10"
            if sound:
                back = ff("-i", path, "-vn", "-ac", "1", "-f", "s16le", "-")
                back = struct.unpack("<%dh" % (len(back) // 2), back)
                rate = int(s["audio"]["sample_rate"])
                c = crossings(back, rate)
                assert 430 <= c <= 450, (fmt, c)
                line += f", {acodec} {rate} Hz {c} Hz tone"
            print(line + f", {len(out)} bytes in {took * 1000:.0f} ms")

    # ---- refused -----------------------------------------------------------------
    BAD, HOST = -2, -5
    nothing = [None] * 4

    def begin(blob):
        return me.call(me.BEGIN, 0, [0] * 4, 0, [blob, None, None, None], [0] * 4)[:2]

    assert begin(me.settings("XYZ ", 8, 8))[0] == BAD
    assert begin(me.settings("HEIC", 0, 8))[0] == BAD
    assert begin(me.settings("HEIC", 9000, 8))[0] == BAD
    assert begin(me.settings("HEIC", 8, 8, pixels=7))[0] == BAD
    assert begin(me.settings("M4A ", rate=22050, channels=3))[0] == BAD
    assert begin(me.settings("HEVC", 8, 8, fps1000=0))[0] == BAD
    assert begin(struct.pack(">I", 2) + me.settings("HEIC", 8, 8)[4:])[0] == BAD
    assert begin(me.settings("HEIC", 8, 8)[:40])[0] == BAD
    st, job = begin(me.settings("HEIC", 8, 8))
    assert st == 0
    put = lambda stream, data: me.call(me.PUT, job, [stream, 0, 0, 0], 0, [data, None, None, None], [0] * 4)[:2]
    assert put(me.STREAM_SOUND, b"\0" * 4)[0] == BAD                  # a picture has no sound
    assert put(me.STREAM_PICTURE, b"\0" * 200) == (0, 200)
    assert put(me.STREAM_PICTURE, b"\0" * 100)[0] == BAD              # more than 8 x 8 x 4
    assert me.call(me.READ, job, [0] * 4, 2, nothing, [0, 64, 0, 0])[0] == BAD     # not finished
    assert me.call(me.FINISH, job, [0] * 4, 2, nothing, [0, 24, 0, 0])[0] == BAD   # 200 of 256 bytes
    assert me.call(me.CLOSE, job, [0] * 4, 0, nothing, [0] * 4)[0] == 0
    assert put(me.STREAM_PICTURE, b"\0")[0] == BAD                    # closed
    assert me.call(me.CLOSE, job, [0] * 4, 0, nothing, [0] * 4)[0] == BAD
    st, job = begin(me.settings("WMA ", rate=22050, channels=2))
    assert put(me.STREAM_SOUND, b"\0" * 3)[0] == 0
    assert me.call(me.FINISH, job, [0] * 4, 2, nothing, [0, 24, 0, 0])[0] == BAD   # half a sample frame
    me.call(me.CLOSE, job, [0] * 4, 0, nothing, [0] * 4)
    # a ninth job sends the oldest away
    jobs = [begin(me.settings("AAC ", rate=8000, channels=1))[1] for _ in range(9)]
    assert me.call(me.PUT, jobs[0], [1, 0, 0, 0], 0, [b"\0\0", None, None, None], [0] * 4)[0] == BAD
    assert me.call(me.PUT, jobs[8], [1, 0, 0, 0], 0, [b"\0\0", None, None, None], [0] * 4)[0] == 0
    for j in jobs[1:]:
        assert me.call(me.CLOSE, j, [0] * 4, 0, nothing, [0] * 4)[0] == 0
    # FINISH answers within its wait while FFmpeg works on, and is asked again; a CLOSE meanwhile
    # lets the job go when FFmpeg is done
    os.environ["OPENSERVICE_ENCODE_WAIT"] = "1"
    film = me.settings("HEVC", 320, 240, me.PIX_RGB, fps1000=25000)
    st, info, out = me.encode(film, pictures=bytes(320 * 240 * 3 * 25))
    assert st == 0 and info["frames"] == 25 and info["polls"] > 0 and len(out) == info["bytes"], (st, info)
    print(f"a slow FINISH: answered 'still working' {info['polls']} times, then the film")
    st, job = begin(film)
    assert put(me.STREAM_PICTURE, bytes(320 * 240 * 3 * 25))[0] == 0
    st, size, working, _ = me.call(me.FINISH, job, [0] * 4, 2, nothing, [0, 24, 0, 0])
    assert (st, size, working) == (0, 0, 1), (st, size, working)
    assert me.call(me.CLOSE, job, [0] * 4, 0, nothing, [0] * 4)[0] == 0
    assert me.call(me.FINISH, job, [0] * 4, 2, nothing, [0, 24, 0, 0])[0] == BAD
    del os.environ["OPENSERVICE_ENCODE_WAIT"]
    later = [begin(me.settings("AAC ", rate=8000, channels=1))[1] for _ in range(8)]   # the closed one keeps its slot
    assert all(later)
    for j in later:
        me.call(me.CLOSE, j, [0] * 4, 0, nothing, [0] * 4)
    # FORMATS with too little room says so, and how many there are
    st, n, _, w = me.call(me.FORMATS, 0, [0] * 4, 2, nothing, [0, 8, 0, 0])
    assert (st, n, len(w[1])) == (-4, 6, 8)
    # no FFmpeg: nothing offered, nothing begun
    os.environ["OPENSERVICE_FFMPEG"] = os.path.join(tmp, "no-such-ffmpeg")
    assert me.formats() == [] and begin(me.settings("HEIC", 8, 8))[0] == BAD
    del os.environ["OPENSERVICE_FFMPEG"]
    assert len(me.formats()) == 6
    print("refused: unknown format, bad sizes, settings version and length, wrong stream, too many bytes, "
          "unfinished reads, short pictures and sample frames, closed and evicted jobs, no FFmpeg")
