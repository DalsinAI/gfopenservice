"""Checks media.decode/1's streaming calls (docs/MEDIA_DECODE.md): HAM8
frames against ffmpeg's own pictures, VSOUND's sound against the tone that
was put in, a video sent in parts with VUPLOAD, and VOPENURL over HTTP for
a plain file and an HLS playlist.
MIT, Copyright (c) 2026 AmiCradle Team."""
import functools
import http.server
import math
import os
import struct
import subprocess
import tempfile
import threading
import media_decode as md

VOPEN, VFRAME, VCLOSE, VOPENURL, VSOUND, VUPLOAD = 3, 4, 5, 6, 7, 8
LIVE, SOUND = 8, 2
W, H = 320, 240


def ham8_show(data, w, h):
    """What an AGA screen shows for VFRAME's HAM8 answer: R, G, B bytes."""
    pal, pix = data[:192], data[192:]
    out = bytearray()
    for y in range(h):
        r, g, b = pal[0], pal[1], pal[2]
        for p in pix[y * w:(y + 1) * w]:
            c, v = p >> 6, p & 63
            if c == 0:
                r, g, b = pal[v * 3], pal[v * 3 + 1], pal[v * 3 + 2]
            elif c == 1:
                b = v << 2 | (b & 3)
            elif c == 2:
                r = v << 2 | (r & 3)
            else:
                g = v << 2 | (g & 3)
            out += bytes((r, g, b))
    return bytes(out)


def cube_show(data):
    out = bytearray()
    for i in data:
        out += bytes(((i // 36) * 51, (i // 6 % 6) * 51, (i % 6) * 51))
    return bytes(out)


def psnr(a, b):
    se = sum((x - y) * (x - y) for x, y in zip(a, b)) / len(a)
    return 99.0 if not se else 10 * math.log10(255 * 255 / se)


def frames_ref(path, w, h):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", f"scale={w}:{h}:flags=area",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    n = w * h * 3
    return [raw[i:i + n] for i in range(0, len(raw) - n + 1, n)]


def tone_hz(samples, rate):
    """The tone's pitch from its rising zero crossings."""
    s = [x - 256 if x > 127 else x for x in samples]
    ups = [i for i in range(1, len(s)) if s[i - 1] < 0 <= s[i]]
    if len(ups) < 2:
        return 0
    return (len(ups) - 1) * rate / (ups[-1] - ups[0])


def vopen(data, extra=(W, H, 0, 0)):
    st, handle, fps, out = md.call(VOPEN, 0, list(extra), 2, [data, None, None, None], [0, 24, 0, 0])
    assert st == 0, st
    return handle, fps, struct.unpack(">6I", out[1])


def frame(handle, i, mode, fixed=0, w=W, h=H):
    room = {0: w * h, 1: w * h * 3, 2: 192 + w * h}[mode]
    st, got, _, out = md.call(VFRAME, handle, [i, mode, fixed, 0], 2, [None] * 4, [0, room, 0, 0])
    assert st == 0, (i, mode, st)
    return got, out[1]


def sound(handle, i, rate):
    st, n, r, out = md.call(VSOUND, handle, [i, rate, 0, 0], 2, [None] * 4, [0, 4096, 0, 0])
    assert st == 0 and r == rate and len(out[1]) == n, (i, st, n, r)
    return out[1]


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


with tempfile.TemporaryDirectory() as tmp:
    clip = os.path.join(tmp, "clip.mp4")
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=s={W}x{H}:r=25:d=4",
                    "-f", "lavfi", "-i", "sine=frequency=440:duration=4:sample_rate=44100",
                    "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", clip], check=True)
    data = open(clip, "rb").read()
    ref = frames_ref(clip, W, H)

    # HAM8 against the 256-colour cube, on the same frames.
    handle, fps, info = vopen(data)
    assert info[0] == 2 and info[2] & SOUND and info[3] == 100 and fps == 25000, (info, fps)
    worst = {"cube": 99, "ham8": 99, "ham8 fixed": 99}
    for i in (0, 37, 99):
        _, cube = frame(handle, i, 0)
        _, ham = frame(handle, i, 2)
        _, hamf = frame(handle, i, 2, 1)
        assert hamf[:12] == bytes((0, 0, 0, 0, 0, 85, 0, 0, 170, 0, 0, 255)), hamf[:12]
        worst["cube"] = min(worst["cube"], psnr(cube_show(cube), ref[i]))
        worst["ham8"] = min(worst["ham8"], psnr(ham8_show(ham, W, H), ref[i]))
        worst["ham8 fixed"] = min(worst["ham8 fixed"], psnr(ham8_show(hamf, W, H), ref[i]))
    assert worst["ham8"] > worst["cube"] + 4 and worst["ham8 fixed"] > worst["cube"] + 2, worst
    print("HAM8: " + ", ".join(f"{k} {v:.1f} dB" for k, v in worst.items()) + " (worst of three frames against ffmpeg)")

    # VSOUND: every frame's share of the tone, in order and out of it.
    rate = 22050
    whole = b"".join(sound(handle, i, rate) for i in range(100))
    assert abs(len(whole) - 4 * rate) <= 1, len(whole)
    hz = tone_hz(whole[rate // 10:-rate // 10], rate)
    assert abs(hz - 440) < 3, hz

    def near(a, b):
        """Within a step or two: a seek starts the decoder and resampler afresh."""
        s8 = lambda x: x - 256 if x > 127 else x
        return len(a) == len(b) and max(abs(s8(x) - s8(y)) for x, y in zip(a, b)) <= 3

    again = sound(handle, 98, rate)
    assert again == whole[98 * rate // 25:99 * rate // 25], "frame 98's sound asked again"
    back = sound(handle, 5, rate)
    assert near(back, whole[5 * rate // 25:6 * rate // 25]), "frame 5's sound after a seek back"
    ahead = sound(handle, 80, rate)
    assert near(ahead, whole[80 * rate // 25:81 * rate // 25]), "frame 80's sound after a seek ahead"
    print(f"VSOUND: {len(whole)} samples at {rate} Hz for 100 frames, a {hz:.1f} Hz tone; "
          "asked again and after a seek back, the same samples")
    assert md.call(VCLOSE, handle, [0] * 4, 0, [None] * 4, [0] * 4)[0] == 0

    # VUPLOAD: the same file in 64 KB parts.
    up, part = 0, 65536
    for k in range(0, len(data), part):
        last = 1 if k + part >= len(data) else 0
        st, up, total, _ = md.call(VUPLOAD, up, [last, 0, 0, 0], 0, [data[k:k + part], None, None, None], [0] * 4)
        assert st == 0 and total == min(k + part, len(data)), (st, total)
    handle2, _, info2 = vopen(b"", (W, H, up, 0))
    assert info2 == info, (info2, info)
    for i in (0, 50, 99):
        assert frame(handle2, i, 1) == (i, ref[i]) or psnr(frame(handle2, i, 1)[1], ref[i]) > 45
    assert md.call(VOPEN, 0, [W, H, up, 0], 2, [b"", None, None, None], [0, 24, 0, 0])[0] == -2, "an upload opens once"
    md.call(VCLOSE, handle2, [0] * 4, 0, [None] * 4, [0] * 4)
    print(f"VUPLOAD: {len(data)} bytes in {(len(data) + part - 1) // part} parts open as the whole file does")

    # VOPENURL over HTTP (a server that takes no ranges, as a stream is):
    # the film with its index first, a transport stream, then an HLS playlist.
    subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-c", "copy", "-movflags", "+faststart",
                    os.path.join(tmp, "film.mp4")], check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-c", "copy", os.path.join(tmp, "channel.ts")], check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-c", "copy", "-f", "hls", "-hls_time", "1",
                    "-hls_playlist_type", "vod", os.path.join(tmp, "live.m3u8")], check=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=tmp))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}/"
    for name in ("film.mp4", "channel.ts", "live.m3u8"):
        st, h3, fps3, out = md.call(VOPENURL, 0, [W, H, 0, 0], 2, [(base + name).encode(), None, None, None], [0, 24, 0, 0])
        assert st == 0, (name, st)
        kind, fmt, flags, frames, ow, oh = struct.unpack(">6I", out[1])
        assert flags & LIVE and flags & SOUND and (ow, oh) == (W, H) and fps3 == 25000, (name, flags, ow, oh, fps3)
        worst = 99
        for i in range(0, 100, 3):                     # forward, by the clock, skipping some
            got, pix = frame(h3, i, 1)
            assert got == i
            worst = min(worst, psnr(pix, ref[i]))
        assert worst > 40, (name, worst)
        got, pix = frame(h3, 10, 1)                    # behind: the last frame again, never back
        assert psnr(pix, ref[99]) > 40, name
        md.call(VCLOSE, h3, [0] * 4, 0, [None] * 4, [0] * 4)
        st, h3, _, out = md.call(VOPENURL, 0, [W, H, 0, 0], 2, [(base + name).encode(), None, None, None], [0, 24, 0, 0])
        tone = b""
        for i in range(60):
            frame(h3, i, 0)
            tone += sound(h3, i, rate)
        hz = tone_hz(tone[rate // 5:], rate)
        assert abs(hz - 440) < 3, (name, hz)
        md.call(VCLOSE, h3, [0] * 4, 0, [None] * 4, [0] * 4)
        print(f"VOPENURL {name}: {frames} frames, live flag, frames by the clock within {worst:.1f} dB of ffmpeg; "
              f"sound a {hz:.1f} Hz tone")
    server.shutdown()

    # Only the network: no files of the host's, no spaces, no other schemes.
    for bad in (b"file:///etc/passwd", b"/etc/passwd", b"http://a b", b"ftp://example.com/x", b""):
        st = md.call(VOPENURL, 0, [W, H, 0, 0], 2, [bad, None, None, None], [0, 24, 0, 0])[0]
        assert st == -2, (bad, st)
    st = md.call(VOPENURL, 0, [W, H, 0, 0], 2, [b"http://127.0.0.1:9/none.ts", None, None, None], [0, 24, 0, 0])[0]
    assert st == -2, st
    print("VOPENURL refuses file:, other schemes, spaces, an empty URL and a server that is not there")
