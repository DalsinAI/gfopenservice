"""Checks media.decode/1's tunes: MIDI through FluidSynth, SID through
sidplayfp and tracker modules through FFmpeg's libopenmpt, each answered
as 16-bit PCM like any other sound. The files are made here, byte by byte.
MIT, Copyright (c) 2026 AmiCradle Team."""
import os
import struct
import tempfile
import time

PROBE, DECODE = 1, 2


def midi():
    """Middle C, then E, then G, half a second each, on a piano."""
    ev = b"\x00\xc0\x00"                                     # program 0
    for note in (60, 64, 67):
        ev += b"\x00\x90" + bytes((note, 100)) + b"\x83\x60\x80" + bytes((note, 0))   # 480 ticks on
    ev += b"\x00\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(ev)) + ev


def sid():
    """A PSID whose init starts a triangle tone on voice 1 and whose play does nothing."""
    code = bytes((0x4c, 0x04, 0x10,                  # $1000 JMP $1004 (init)
                  0x60,                              # $1003 RTS (play)
                  0xa9, 0x0f, 0x8d, 0x18, 0xd4,      # LDA #$0F, STA $D418 volume
                  0xa9, 0x00, 0x8d, 0x05, 0xd4,      # attack/decay 0
                  0xa9, 0xf0, 0x8d, 0x06, 0xd4,      # sustain 15, release 0
                  0xa9, 0x25, 0x8d, 0x01, 0xd4,      # frequency high
                  0xa9, 0x11, 0x8d, 0x04, 0xd4,      # triangle, gate on
                  0x60))
    head = b"PSID" + struct.pack(">HHHHHHHI", 2, 0x7c, 0, 0x1000, 0x1003, 1, 1, 0)
    head += b"GFOpenService test".ljust(32, b"\0") + b"AmiCradle Team".ljust(32, b"\0") + b"2026".ljust(32, b"\0")
    head += struct.pack(">HBBH", 0, 0, 0, 0)
    return head + struct.pack("<H", 0x1000) + code


def mod():
    """A 4-channel M.K. module: one looping square wave, one note, one pattern."""
    out = b"GFOpenService test".ljust(20, b"\0")
    out += b"square".ljust(22, b"\0") + struct.pack(">HBBHH", 32, 0, 64, 0, 32)
    out += (b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)) * 30
    out += bytes((1, 127)) + bytes(128) + b"M.K."
    pattern = bytearray(64 * 16)
    pattern[0:4] = bytes((0x00 | (428 >> 8), 428 & 0xff, 0x10, 0x00))
    out += bytes(pattern) + bytes([100] * 32 + [156] * 32)
    return out


def call(op, data, maxch=2, maxrate=28000, room=0, first=0):
    return md.call(op, first, [maxch, maxrate, 0, 0], 2, [data, None, None, None], [0, room, 0, 0])


# The work files on a RAM disk and the cache on the real one, as on most
# Cradle PCs, so the result has to cross filesystems.
work = tempfile.TemporaryDirectory(dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
os.environ["TMPDIR"] = work.name
with tempfile.TemporaryDirectory(dir="/var/tmp") as tmp:
    os.environ["OPENSERVICE_CACHE"] = os.path.join(tmp, "cache")
    os.environ["OPENSERVICE_SIDSECONDS"] = "3"
    import media_decode as md
    for name, data, fmt, least in (("tune.mid", midi(), b"MIDI", 1.4), ("tune.sid", sid(), b"SID ", 2.9),
                                   ("tune.mod", mod(), b"MOD ", 1.0)):
        t = time.perf_counter()
        st, frames, rate, out = call(PROBE, data, room=24)
        first = time.perf_counter() - t
        assert st == 0, (name, st)
        kind, f, flags, nframes, orate, ochan = struct.unpack(">6I", out[1])
        assert kind == 3 and f == struct.unpack(">I", fmt)[0], (name, kind, struct.pack(">I", f))
        assert nframes / orate >= least and orate <= 28000, (name, nframes, orate)
        t = time.perf_counter()
        st, n, _, out = call(DECODE, data, room=nframes * ochan * 2)
        again = time.perf_counter() - t
        assert st == 0 and n == nframes, (name, st, n)
        pcm = struct.unpack(">%dh" % (n * ochan), out[1])
        peak = max(abs(v) for v in pcm)
        assert peak > 1000, (name, peak)
        print(f"{name}: {fmt.decode()} {nframes / orate:.1f} s at {orate} Hz x{ochan}, peak {peak}; "
              f"first probe {first:.2f} s, decode after {again:.2f} s")
    assert call(PROBE, b"MThd" + bytes(20), room=24)[0] == -2
    print("bad tune refused")
