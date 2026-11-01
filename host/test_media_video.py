"""Checks media_av.c's video against ffmpeg's own decoding: a grey ramp,
one shade a frame, in several containers and codecs. Frames asked for in
order, backwards and at random must be the right ones.
MIT, Copyright (c) 2026 AmiCradle Team."""
import os
import random
import struct
import subprocess
import tempfile
import media_decode as md

VOPEN, VFRAME, VCLOSE = 3, 4, 5
CLIPS = (("mp4", ["-c:v", "libx264", "-g", "12", "-c:a", "aac"], b"H264", True),
         ("mkv", ["-c:v", "libx265", "-x265-params", "log-level=error", "-g", "12", "-an"], b"HEVC", False),
         ("ivf", ["-c:v", "libaom-av1", "-cpu-used", "8", "-g", "12", "-an"], b"AV1 ", False),
         ("avi", ["-c:v", "mpeg4", "-g", "12", "-an"], b"MPG4", False),
         ("wmv", ["-c:v", "wmv2", "-g", "12", "-c:a", "wmav2"], b"WMV ", True),
         ("webm", ["-c:v", "libvpx-vp9", "-g", "12", "-an"], b"VP9 ", False))


def means(raw, w, h):
    n = w * h * 3
    return [sum(raw[i:i + n]) / n for i in range(0, len(raw) - n + 1, n)]


with tempfile.TemporaryDirectory() as tmp:
    for ext, args, fourcc, sound in CLIPS:
        path = os.path.join(tmp, "ramp." + ext)
        cmd = ["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
               "nullsrc=s=320x240:r=10:d=4,geq=lum='16+N*5':cb=128:cr=128,format=yuv420p"]
        if sound:
            cmd += ["-f", "lavfi", "-i", "sine=duration=4"]
        subprocess.run(cmd + args + ["-pix_fmt", "yuv420p", path], check=True)
        data = open(path, "rb").read()
        st, handle, fps, out = md.call(VOPEN, 0, [160, 160, 0, 0], 2, [data, None, None, None], [0, 24, 0, 0])
        assert st == 0, (ext, st)
        kind, fmt, flags, frames, ow, oh = struct.unpack(">6I", out[1])
        assert (kind, fmt, ow, oh, fps) == (2, struct.unpack(">I", fourcc)[0], 160, 120, 10000), (ext, kind, hex(fmt), ow, oh, fps)
        assert frames == 40 and bool(flags & 2) == sound, (ext, frames, flags)
        ref = means(subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", "scale=160:120:flags=area",
                                    "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout, 160, 120)
        order = list(range(40)) + [39, 20, 5, 0, 33] + random.Random(7).sample(range(40), 15)
        worst = 0
        for i in order:
            st, got, _, out = md.call(VFRAME, handle, [i, 1, 0, 0], 2, [None] * 4, [0, 160 * 120 * 3, 0, 0])
            assert st == 0 and got == i and len(out[1]) == 160 * 120 * 3, (ext, i, st, got)
            m = sum(out[1]) / len(out[1])
            worst = max(worst, abs(m - ref[i]))
            assert abs(m - ref[i]) < 3, (ext, i, m, ref[i])
        st, _, _, out = md.call(VFRAME, handle, [17, 0, 0, 0], 2, [None] * 4, [0, 160 * 120, 0, 0])
        assert st == 0 and len(out[1]) == 160 * 120 and max(out[1]) < 256
        assert md.call(VCLOSE, handle, [0] * 4, 0, [None] * 4, [0] * 4)[0] == 0
        assert md.call(VFRAME, handle, [0, 0, 0, 0], 2, [None] * 4, [0, 160 * 120, 0, 0])[0] == -2
        print(f"{ext}: {fourcc.decode()} {frames} frames at {fps / 1000:g} fps, {ow}x{oh}, sound={sound}; "
              f"{len(order)} frames in and out of order match ffmpeg (worst {worst:.2f})")
