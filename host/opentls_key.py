"""ctypes binding for libopentlskey.so (opentls_key.h), for the LAN Cradle.
MIT, Copyright (c) 2026 AmiCradle Team."""
import ctypes
import os


class Buffer(ctypes.Structure):
    _fields_ = [("inp", ctypes.c_void_p), ("out", ctypes.c_void_p),
                ("length", ctypes.c_uint32), ("written", ctypes.c_uint32)]


_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "libopentlskey.so"))
_lib.otk_call.argtypes = [ctypes.c_uint16, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                          ctypes.POINTER(Buffer), ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
_lib.otk_call.restype = ctypes.c_int


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
    status = _lib.otk_call(op, arg, ex, arr, ctypes.byref(result), ctypes.byref(aux))
    written = {}
    for i in range(4):
        if flags & (1 << i) and lengths[i]:
            written[i] = ctypes.string_at(arr[i].out, arr[i].written)
    return status, result.value, aux.value, written
