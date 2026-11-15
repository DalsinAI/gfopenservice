#!/usr/bin/env python3
"""Timed stand-in acscreen for acm_planetest's COPY/menu regression."""
import array
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ACV_MAGIC = 0x31564341
ACV_HELLO, ACV_RECT, ACV_HIDE, ACV_COPY = 1, 2, 4, 5
ACV_WELCOME, ACV_SHOWN, ACV_DONE, ACV_STATE = 0x81, 0x82, 0x83, 0x84
ACV_WF_COPY, ACV_W_OK, ACV_W_NOT_SHOWN = 1, 0, 1


def packet(kind, body=b""):
    return struct.pack("=IIII", ACV_MAGIC, kind, len(body), 0) + body


class Screen(threading.Thread):
    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.ready = threading.Event()
        self.copies = 0
        self.error = None

    def run(self):
        server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            server.bind(self.path)
            server.listen(1)
            self.ready.set()
            conn, _ = server.accept()
            with conn:
                while True:
                    data, ancillary, _, _ = conn.recvmsg(256, socket.CMSG_SPACE(16))
                    if not data:
                        break
                    magic, kind, length, _ = struct.unpack_from("=IIII", data)
                    if magic != ACV_MAGIC or length != len(data) - 16:
                        raise RuntimeError("bad packet")
                    for level, ctype, value in ancillary:
                        if level == socket.SOL_SOCKET and ctype == socket.SCM_RIGHTS:
                            fds = array.array("i")
                            fds.frombytes(value[: len(value) - len(value) % fds.itemsize])
                            for fd in fds:
                                os.close(fd)
                    body = data[16:]
                    if kind == ACV_HELLO:
                        welcome = struct.pack("=10I", 1, ACV_WF_COPY, 1920, 1080, 60000,
                                              1920, 1080, 1, ACV_W_OK, 0)
                        conn.send(packet(ACV_WELCOME, welcome))
                    elif kind == ACV_RECT:
                        pass
                    elif kind == ACV_COPY:
                        frame_id, due_ns = struct.unpack_from("=QQ", body)
                        delay = (due_ns - time.monotonic_ns()) / 1e9
                        if delay > 0:
                            time.sleep(delay)
                        conn.send(packet(ACV_SHOWN, struct.pack("=QQII", frame_id,
                                                               time.monotonic_ns(), self.copies, 0)))
                        conn.send(packet(ACV_DONE, struct.pack("=QII", frame_id, ACV_W_OK, 0)))
                        self.copies += 1
                        if self.copies == 10:
                            conn.send(packet(ACV_STATE, struct.pack("=4I", 0, ACV_W_NOT_SHOWN,
                                                                   1920, 1080)))
                            time.sleep(0.35)
                            conn.send(packet(ACV_STATE, struct.pack("=4I", 1, ACV_W_OK,
                                                                   1920, 1080)))
                    elif kind == ACV_HIDE:
                        break
                    else:
                        raise RuntimeError(f"unexpected packet {kind}")
        except Exception as exc:  # surfaced to the main test below
            self.error = exc
            self.ready.set()
        finally:
            server.close()


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} ACM_PLANETEST WORKER CLIP", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="acm-planecopy-") as tmp:
        path = os.path.join(tmp, "screen.sock")
        screen = Screen(path)
        screen.start()
        if not screen.ready.wait(2) or screen.error:
            raise RuntimeError(f"stand-in did not start: {screen.error}")
        started = time.monotonic()
        run = subprocess.run([sys.argv[1], sys.argv[2], sys.argv[3],
                              "--acscreen", path, "--copy", "--frames", "60", "--verbose"],
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=15, check=False)
        elapsed = time.monotonic() - started
        screen.join(2)
        print(run.stdout, end="")
        required = ["software COPY route", "frames are COPY", "plane off (1)",
                    "waiting for plane to return", "plane on (0)", "resumed after",
                    "60 sent (60 COPY)", "0 off the plane", "WELCOME copy=yes",
                    "PASS software COPY frames through acscreen"]
        missing = [text for text in required if text not in run.stdout]
        if run.returncode or screen.error or screen.is_alive() or screen.copies != 60 or missing:
            print(f"test_planetest_copy: FAIL rc={run.returncode} copies={screen.copies} "
                  f"missing={missing} server={screen.error}", file=sys.stderr)
            return 1
        if elapsed < 2.2:
            print(f"test_planetest_copy: FAIL unpaced run ended in {elapsed:.2f}s", file=sys.stderr)
            return 1
        print(f"test_planetest_copy: pass (60 COPY, STATE off/on, {elapsed:.2f}s)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
