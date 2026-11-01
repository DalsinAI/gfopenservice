"""ctypes binding for libopenwebfetch.so (openweb_fetch.h), for the LAN Cradle and the tests.
MIT, Copyright (c) 2026 AmiCradle Team."""
import ctypes
import os
import threading
import time


class Buffer(ctypes.Structure):
    _fields_ = [("inp", ctypes.c_void_p), ("out", ctypes.c_void_p),
                ("length", ctypes.c_uint32), ("written", ctypes.c_uint32)]


_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "libopenwebfetch.so"))
_lib.wf_call.argtypes = [ctypes.c_uint16, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                         ctypes.POINTER(Buffer), ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
_lib.wf_call.restype = ctypes.c_int


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
    status = _lib.wf_call(op, arg, ex, arr, ctypes.byref(result), ctypes.byref(aux))
    written = {}
    for i in range(4):
        if flags & (1 << i) and lengths[i]:
            written[i] = ctypes.string_at(arr[i].out, arr[i].written)
    return status, result.value, aux.value, written


# ---- answering later (wf_start, wf_cancel), as the services card calls them ----

DONE = ctypes.CFUNCTYPE(None, ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint32, ctypes.c_uint32)
_lib.wf_start.argtypes = [ctypes.c_uint16, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(Buffer),
                          ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, DONE]
_lib.wf_start.restype = ctypes.c_int
_lib.wf_cancel.argtypes = [ctypes.c_uint32]
_lib.wf_cancel.restype = None
LATER = 1


class Card:
    """HEAD and READ through wf_start, as the services card makes them: each answered now, or
    later through done, from one of the library's threads, by the id given."""

    def __init__(self):
        self.cv = threading.Condition()
        self.out = {}                   # id -> its out buffer, alive until answered
        self.answers = {}               # id -> [(status, result, aux, the buffer's bytes, time)], one per done
        self.next_id = 1
        self.done = DONE(self._done)    # kept: the library calls it from its threads

    def _done(self, ident, status, result, aux):
        with self.cv:
            out = self.out.pop(ident, None)
            self.answers.setdefault(ident, []).append((status, result, aux, out.raw if out is not None else b"", time.time()))
            self.cv.notify_all()

    def start(self, op, arg, wait, room):
        """(id, status, result, aux, bytes written); status LATER when the answer comes through done."""
        with self.cv:
            ident = self.next_id
            self.next_id += 1
            out = ctypes.create_string_buffer(max(room, 1))
            self.out[ident] = out
        arr = (Buffer * 4)()
        if room:
            arr[0].out = ctypes.cast(out, ctypes.c_void_p)
            arr[0].length = room
        ex = (ctypes.c_uint32 * 4)(wait, 0, 0, 0)
        result, aux = ctypes.c_uint32(), ctypes.c_uint32()
        st = _lib.wf_start(op, arg, ex, arr, ctypes.byref(result), ctypes.byref(aux), ident, self.done)
        if st == LATER:
            return ident, st, 0, 0, b""
        with self.cv:
            self.out.pop(ident, None)
        return ident, st, result.value, aux.value, out.raw[:arr[0].written]

    def wait(self, ident, timeout=10):
        """The first answer through done: (status, result, aux, the buffer, time), or None."""
        with self.cv:
            self.cv.wait_for(lambda: ident in self.answers, timeout)
            return self.answers.get(ident, [None])[0]

    def count(self, ident):
        with self.cv:
            return len(self.answers.get(ident, []))

    def cancel(self, ident):
        _lib.wf_cancel(ident)
