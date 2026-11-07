"""ctypes binding for libmediaencode.so (media_encode.h, media.encode/1),
for the LAN Cradle and the tests; and a small writer on top of it.
MIT, Copyright (c) 2026 AmiCradle Team."""
import ctypes
import os
import struct

FORMATS, BEGIN, PUT, FINISH, READ, CLOSE = 16, 17, 18, 19, 20, 21
STREAM_PICTURE, STREAM_SOUND = 0, 1
PIX_ARGB, PIX_RGB = 0, 1
SAMPLE_S16, SAMPLE_S8 = 0, 1
F_ALPHA = 1
SETTINGS_SIZE, INFO_SIZE = 48, 24


def fourcc(s):
    return int.from_bytes(s.ljust(4).encode(), "big")


class Buffer(ctypes.Structure):
    _fields_ = [("inp", ctypes.c_void_p), ("out", ctypes.c_void_p),
                ("length", ctypes.c_uint32), ("written", ctypes.c_uint32)]


_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "libmediaencode.so"))
_lib.me_call.argtypes = [ctypes.c_uint16, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                         ctypes.POINTER(Buffer), ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
_lib.me_call.restype = ctypes.c_int


def call(op, arg, extra, flags, bufs, lengths):
    """bufs[i]: bytes the service reads, or None; lengths[i]: room for written ones.
    Returns (status, result, aux, {i: bytes written})."""
    keep = []
    arr = (Buffer * 4)()
    for i in range(4):
        if flags & (1 << i) and lengths[i]:
            out = ctypes.create_string_buffer(lengths[i])
            keep.append(out)
            arr[i].out = ctypes.cast(out, ctypes.c_void_p)
            arr[i].length = lengths[i]
        elif bufs[i] is not None:
            data = ctypes.create_string_buffer(bytes(bufs[i]), len(bufs[i]) or 1)
            keep.append(data)
            arr[i].inp = ctypes.cast(data, ctypes.c_void_p)
            arr[i].length = len(bufs[i])
    ex = (ctypes.c_uint32 * 4)(*extra)
    result, aux = ctypes.c_uint32(), ctypes.c_uint32()
    status = _lib.me_call(op, arg, ex, arr, ctypes.byref(result), ctypes.byref(aux))
    written = {}
    for i in range(4):
        if flags & (1 << i) and lengths[i]:
            written[i] = ctypes.string_at(arr[i].out, arr[i].written)
    return status, result.value, aux.value, written


def formats():
    st, n, _, w = call(FORMATS, 0, [0] * 4, 2, [None] * 4, [0, 64, 0, 0])
    data = w.get(1, b"")
    return [data[i:i + 4].decode() for i in range(0, len(data), 4)] if st == 0 else []


def settings(fmt, width=0, height=0, pixels=PIX_ARGB, fps1000=0, rate=0, channels=0, samples=SAMPLE_S16,
             quality=0, flags=0):
    return struct.pack(">12I", 1, fourcc(fmt), width, height, pixels, fps1000, rate, channels, samples, quality,
                       flags, 0)


def encode(blob, pictures=b"", sound=b"", piece=256 * 1024):
    """BEGIN with the settings blob, PUT the bytes in pieces, FINISH, READ the
    file back. Returns (status, info dict, file bytes)."""
    st, job, _, _ = call(BEGIN, 0, [0] * 4, 0, [blob, None, None, None], [0] * 4)
    if st:
        return st, None, b""
    try:
        for stream, data in ((STREAM_PICTURE, pictures), (STREAM_SOUND, sound)):
            for at in range(0, len(data), piece):
                st = call(PUT, job, [stream, 0, 0, 0], 0, [data[at:at + piece], None, None, None], [0] * 4)[0]
                if st:
                    return st, None, b""
        polls = 0
        while True:                                  # aux 1: FFmpeg still at work
            st, size, working, w = call(FINISH, job, [0] * 4, 2, [None] * 4, [0, INFO_SIZE, 0, 0])
            if st or not working:
                break
            polls += 1
        if st:
            return st, None, b""
        v = struct.unpack(">6I", w[1])
        info = dict(format=v[0].to_bytes(4, "big").decode(), bytes=v[1], width=v[2], height=v[3], frames=v[4],
                    sample_frames=v[5], polls=polls)
        out = b""
        while len(out) < size:
            st, n, _, w = call(READ, job, [len(out), 0, 0, 0], 2, [None] * 4, [0, piece, 0, 0])
            if st or not n:
                break
            out += w[1]
        return st, info, out
    finally:
        call(CLOSE, job, [0] * 4, 0, [None] * 4, [0] * 4)
