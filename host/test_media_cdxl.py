"""Checks media.cdxl/1 (cdxl.c): a three-second movie with a 440 Hz tone,
turned into CDXL with each preset, read back by FFmpeg's own CDXL reader.
MIT, Copyright (c) 2026 AmiCradle Team."""
import json
import os
import struct
import subprocess
import tempfile
import time

CONVERT, READ, CLOSE = 1, 2, 3


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-f", "cdxl", "-count_frames", "-show_streams", "-of", "json", path],
                         check=True, capture_output=True).stdout
    return {s["codec_type"]: s for s in json.loads(out)["streams"]}


def psnr(cdxl, still, size):
    out = subprocess.run(["ffmpeg", "-nostats", "-f", "cdxl", "-i", cdxl, "-i", still, "-filter_complex",
                          f"[0:v]format=rgb24,select=eq(n\\,0)[a];[1:v]scale={size}:flags=area,format=rgb24[b];[a][b]psnr",
                          "-frames:v", "1", "-f", "null", "-"], capture_output=True, text=True).stderr
    return float(out.split("average:")[1].split()[0])


with tempfile.TemporaryDirectory() as tmp:
    os.environ["OPENSERVICE_CACHE"] = os.path.join(tmp, "cache")
    import media_cdxl as cx
    import media_decode as md
    movie, still, steady = (os.path.join(tmp, n) for n in ("m.mp4", "still.png", "s.mp4"))
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3",
                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=3", "-c:v", "libx264",
                    "-c:a", "aac", "-shortest", movie], check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "mandelbrot=size=640x360", "-frames:v", "1", still],
                   check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-loop", "1", "-i", still, "-t", "1", "-r", "25", "-c:v", "libx264",
                    "-pix_fmt", "yuv444p", "-qp", "0", steady], check=True)
    data, calm = open(movie, "rb").read(), open(steady, "rb").read()
    #          preset     pix_fmt  size        fps rate   ch least PSNR
    for preset, pix, size, fps, rate, ch, least in (
            ("ecs", "pal8", "320x180", 12, 11016, 1, 22), ("ecs-ham", "bgr24", "320x180", 12, 11016, 1, 26),
            ("aga", "pal8", "320x180", 15, 22050, 1, 25), ("aga-ham", "bgr24", "320x180", 15, 22050, 1, 32),
            ("rtg", "rgb24", "640x360", 25, 22050, 2, 40)):
        t = time.perf_counter()
        st, info, cdxl = cx.convert(data, cx.settings(preset))
        took = time.perf_counter() - t
        assert st == 0 and len(cdxl) == info["bytes"], (preset, st)
        path = os.path.join(tmp, preset + ".cdxl")
        open(path, "wb").write(cdxl)
        s = probe(path)
        v, a = s["video"], s["audio"]
        assert (v["pix_fmt"], f"{v['width']}x{v['height']}", v["avg_frame_rate"]) == (pix, size, f"{fps}/1"), (preset, v)
        assert int(v["nb_read_frames"]) == 3 * fps == info["frames"], (preset, v["nb_read_frames"])
        assert (int(a["sample_rate"]), a["channels"]) == (rate, ch), (preset, a)
        pcm = subprocess.run(["ffmpeg", "-v", "error", "-f", "cdxl", "-i", path, "-vn", "-f", "s16le", "-ac", "1", "-"],
                             check=True, capture_output=True).stdout
        pcm = struct.unpack("<%dh" % (len(pcm) // 2), pcm)
        mid = pcm[rate:rate + rate // 10]
        cross = sum(1 for x, y in zip(mid, mid[1:]) if x < 0 <= y)
        assert abs(len(pcm) / rate - 3) < 0.05 and 42 <= cross <= 46 and max(mid) > 2000, (preset, len(pcm), cross)
        st, info2, _ = cx.convert(calm, cx.settings(preset))
        open(path, "wb").write(_)
        q = psnr(path, still, size)
        assert st == 0 and q >= least, (preset, q)
        t = time.perf_counter()
        assert cx.convert(data, cx.settings(preset))[0] == 0
        cached = time.perf_counter() - t
        print(f"{preset}: {info['frames']} frames {size} {pix}, {rate} Hz x{ch}, 440 Hz tone ok, "
              f"still {q:.1f} dB, {len(cdxl)} bytes, {took:.2f} s ({cached:.2f} s cached)")

    st, info, cdxl = cx.convert(data, cx.settings("aga", size="256x200", fps=10, planes=4, sound=False))
    path = os.path.join(tmp, "custom.cdxl")
    open(path, "wb").write(cdxl)
    s = probe(path)
    assert "audio" not in s and (s["video"]["width"], s["video"]["height"]) == (256, 144), s
    assert cdxl[19] == 4 and struct.unpack(">H", cdxl[20:22])[0] == 32 and info["frames"] == 30, cdxl[:32]
    print("custom: 256x144, 16 colours, 10 fps, no sound")

    assert md.cdxl_call(CONVERT, 0, [0, 0, 0, 0], 2, [b"not a movie" * 100, None, None, None], [0, 24, 0, 0])[0] == -2
    assert md.cdxl_call(CONVERT, 0, [9, 0, 0, 0], 2, [data, None, None, None], [0, 24, 0, 0])[0] == -2
    assert md.cdxl_call(READ, 7, [0, 0, 0, 0], 2, [None] * 4, [0, 64, 0, 0])[0] == -2
    print("bad movie, bad preset and bad handle refused")
