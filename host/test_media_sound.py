"""Checks media_av.c's sounds against files made by ffmpeg: a 1 kHz tone,
44.1 kHz stereo, two seconds. MIT, Copyright (c) 2026 AmiCradle Team."""
import math
import os
import struct
import subprocess
import tempfile
import media_decode as md

PROBE, DECODE = 1, 2
FORMATS = {"flac": b"FLAC", "ogg": b"VORB", "opus": b"OPUS", "mp3": b"MP3 ", "m4a": b"AAC ", "wav": b"SOUN"}


def call(op, data, maxch=0, maxrate=0, room=0, first=0):
    return md.call(op, first, [maxch, maxrate, 0, 0], 2, [data, None, None, None], [0, room, 0, 0])


with tempfile.TemporaryDirectory() as tmp:
    for ext, fourcc in FORMATS.items():
        path = os.path.join(tmp, "tone." + ext)
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100:duration=2",
                        "-ac", "2", path], check=True)
        data = open(path, "rb").read()
        st, frames, rate, out = call(PROBE, data, 2, 28000, room=24)
        kind, fmt, flags, nframes, orate, ochan = struct.unpack(">6I", out[1])
        assert st == 0 and kind == 3 and fmt == struct.unpack(">I", fourcc)[0], (ext, st, kind, hex(fmt))
        want = 24000 if ext == "opus" else 22050                # Opus always decodes at 48 kHz
        assert (orate, ochan, rate) == (want, 2, want), (ext, orate, ochan)
        assert abs(nframes - 2 * want) < 2000 and frames == nframes, (ext, nframes)
        st, n, rate, out = call(DECODE, data, 2, 28000, room=nframes * 4)
        assert st == 0 and n == nframes and len(out[1]) == n * 4, (ext, st, n)
        pcm = struct.unpack(">%dh" % (n * 2), out[1])
        mid = pcm[2 * want // 2: 2 * want // 2 + 2 * (want // 10)]  # 0.1 s from the middle, left and right
        peak = max(abs(v) for v in mid)
        # a 1 kHz tone: 100 cycles in 0.1 s; rising zero crossings of the left channel
        left = mid[0::2]
        cross = sum(1 for a, b in zip(left, left[1:]) if a < 0 <= b)
        assert peak > 2000 and 95 <= cross <= 105, (ext, peak, cross)
        st, n2, _, out2 = call(DECODE, data, 1, 0, room=1000 * 2, first=1000)
        assert st == 0 and n2 == 1000 and len(out2[1]) == 2000, (ext, st, n2)
        print(f"{ext}: {nframes} frames at {orate} Hz stereo, peak {peak}, {cross} cycles in 0.1 s; mono chunk ok")
    assert call(PROBE, b"definitely not a sound file", room=24)[0] == -2
    print("bad file refused")
