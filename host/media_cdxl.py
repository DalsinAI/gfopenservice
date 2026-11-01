#!/usr/bin/env python3
"""Turns a movie into CDXL on the Cradle, through media.cdxl/1 (cdxl.h,
docs/MEDIA_CDXL.md), the same code an Amiga calls through gfopenservice.device.

usage: media_cdxl.py MOVIE OUT.cdxl [--preset ecs|ecs-ham|aga|aga-ham|rtg]
           [--size WxH] [--fps N] [--planes N] [--rate HZ] [--stereo] [--no-sound]

MIT, Copyright (c) 2026 AmigaChrome Team."""
import argparse
import struct
import sys

import media_decode as md

CONVERT, READ, CLOSE = 1, 2, 3
PRESETS = {"ecs": 0, "ecs-ham": 1, "aga": 2, "aga-ham": 3, "rtg": 4}
MODES = {0: "colours", 1: "HAM", 2: "24-bit chunky"}


def settings(preset="aga", size=None, fps=0, planes=0, rate=0, stereo=False, sound=True):
    """The four extra words CONVERT takes."""
    w, h = (int(v) for v in size.lower().split("x")) if size else (0, 0)
    return [PRESETS[preset], w << 16 | h, (fps & 0xff) | (planes & 0xff) << 8,
            (rate & 0x3ffff) | (1 << 31 if stereo else 0) | (0 if sound else 1 << 30)]


def convert(movie, extra):
    """Returns (status, info dict, the CDXL bytes)."""
    st, handle, size, out = md.cdxl_call(CONVERT, 0, extra, 2, [movie, None, None, None], [0, 24, 0, 0])
    if st:
        return st, None, b""
    magic, frames, w, h, mode, rate = struct.unpack(">6I", out[1])
    info = {"frames": frames, "width": w, "height": h, "planes": mode & 0xff, "mode": MODES[(mode >> 8) & 0xff],
            "stereo": bool(mode >> 16 & 1), "fps": rate >> 24, "rate": rate & 0xffffff, "bytes": size}
    data = bytearray()
    while len(data) < size:
        st, n, _, got = md.cdxl_call(READ, handle, [len(data), 0, 0, 0], 2, [None] * 4, [0, 1 << 20, 0, 0])
        if st or not n:
            break
        data += got[1]
    md.cdxl_call(CLOSE, handle, [0, 0, 0, 0], 0, [None] * 4, [0] * 4)
    return st, info, bytes(data)


def main():
    p = argparse.ArgumentParser(description="Turn a movie into CDXL for an Amiga.")
    p.add_argument("movie")
    p.add_argument("out")
    p.add_argument("--preset", choices=PRESETS, default="aga")
    p.add_argument("--size", help="largest WxH, e.g. 320x256")
    p.add_argument("--fps", type=int, default=0)
    p.add_argument("--planes", type=int, default=0, help="1 to 8, for ecs and aga")
    p.add_argument("--rate", type=int, default=0)
    p.add_argument("--stereo", action="store_true")
    p.add_argument("--no-sound", action="store_true")
    a = p.parse_args()
    extra = settings(a.preset, a.size, a.fps, a.planes, a.rate, a.stereo, not a.no_sound)
    st, info, data = convert(open(a.movie, "rb").read(), extra)
    if st:
        sys.exit(f"media.cdxl/1 refused it (status {st})")
    open(a.out, "wb").write(data)
    print(f"{a.out}: {info['frames']} frames, {info['width']}x{info['height']} {info['mode']} "
          f"({info['planes']} planes), {info['fps']} fps, "
          + (f"{info['rate']} Hz {'stereo' if info['stereo'] else 'mono'}" if info["rate"] else "no sound")
          + f", {len(data)} bytes")


if __name__ == "__main__":
    main()
