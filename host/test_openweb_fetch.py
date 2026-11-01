"""Checks openweb_fetch.c against servers this test starts on 127.0.0.1: HTTP,
HTTPS with a certificate it makes in a temporary directory (and throws away),
errors, cancels, many fetches at once; then https://example.com when the
network allows. MIT, Copyright (c) 2026 AmigaChrome Team."""
import ctypes
import gzip
import hashlib
import http.server
import os
import random
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
import openweb_fetch as wf

OPEN, SEND, HEAD, READ, CANCEL, CLOSE, STATS = range(1, 8)
BODY, DECODE, LAST, UNKNOWN = 1, 2, 1, 0xffffffff
H_TLS, H_VERIFIED, H_DECODED, H_LENGTH, H_NOBODY = 1, 2, 4, 8, 16
E_DNS, E_CONNECT, E_TLS, E_TIMEOUT, E_PROTOCOL, E_CANCELLED, E_CLOSED, E_DECODE = range(1, 9)

BIG = random.Random(1200).randbytes(6 << 20)
TEXT = b"".join(b"Line %d of a page the Amiga asked for.\n" % i for i in range(4000))


def brotli_compress(data):
    """Brotli through the system's libbrotlienc, or None when it is not there."""
    try:
        enc = ctypes.CDLL("libbrotlienc.so.1")
    except OSError:
        return None
    size = ctypes.c_size_t(len(data) + 1024)
    out = ctypes.create_string_buffer(size.value)
    ok = enc.BrotliEncoderCompress(ctypes.c_int(9), ctypes.c_int(22), ctypes.c_int(0), ctypes.c_size_t(len(data)),
                                   data, ctypes.byref(size), out)
    return out.raw[:size.value] if ok else None


BR = brotli_compress(TEXT)


LOCK = threading.Lock()
OPENED, CLOSED = [], []                         # the client ports of connections the servers took and saw end


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def setup(self):
        super().setup()
        # The head and the body go in two sends: without TCP_NODELAY the second waits for the
        # client's delayed ACK, 40 ms an answer on a kept connection. Servers set it (nginx does).
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with LOCK:
            OPENED.append(self.client_address[1])

    def finish(self):
        super().finish()
        with LOCK:
            CLOSED.append(self.client_address[1])

    def reply(self, code, body, headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if code not in (204, 304):
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and code not in (204, 304):
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        p, accept = self.path, self.headers.get("Accept-Encoding", "")
        if p == "/hello":
            self.reply(200, b"hello, Amiga\n", [("Set-Cookie", "a=1; Path=/"), ("Set-Cookie", "b=2"), ("X-Test", "yes")])
        elif p == "/whoami":                                    # which connection: the client's port
            self.reply(200, str(self.client_address[1]).encode())
        elif p == "/slowhead":
            time.sleep(0.5)
            self.reply(200, b"slow head\n")
        elif p == "/slowwho":
            time.sleep(0.3)
            self.reply(200, str(self.client_address[1]).encode())
        elif p == "/ka2":
            self.reply(200, str(self.client_address[1]).encode(), [("Keep-Alive", "timeout=2")])
        elif p == "/closing":
            self.reply(200, str(self.client_address[1]).encode(), [("Connection", "close")])
            self.close_connection = True
        elif p == "/headers":
            self.reply(200, "".join(f"{k}: {v}\n" for k, v in self.headers.items()).encode())
        elif p == "/redirect":
            self.reply(302, b"", [("Location", "/hello")])
        elif p == "/missing":
            self.reply(404, b"no such page\n")
        elif p == "/empty":
            self.reply(204, b"")
        elif p == "/big":
            self.reply(200, BIG, [("Content-Type", "application/octet-stream")])
        elif p == "/gzip":
            if "gzip" in accept:
                self.reply(200, gzip.compress(TEXT), [("Content-Encoding", "gzip")])
            else:
                self.reply(200, TEXT)
        elif p == "/br":
            if BR and "br" in accept:
                self.reply(200, BR, [("Content-Encoding", "br")])
            else:
                self.reply(200, TEXT)
        elif p == "/badbr":
            self.reply(200, b"\xff\xff\xff not brotli at all", [("Content-Encoding", "br")])
        elif p == "/deflate":
            c = zlib.compressobj(wbits=-15)                       # raw, as some servers send it
            self.reply(200, c.compress(TEXT) + c.flush(), [("Content-Encoding", "deflate")])
        elif p == "/badgzip":
            self.reply(200, b"\x1f\x8b\x08\x00not gzip at all, really", [("Content-Encoding", "gzip")])
        elif p == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(TEXT), 7777):
                piece = TEXT[i:i + 7777]
                self.wfile.write(b"%x;ext=1\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\nX-Trailer: 1\r\n\r\n")
        elif p == "/close":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(TEXT)
            self.close_connection = True
        elif p == "/short":
            self.send_response(200)
            self.send_header("Content-Length", "1000")
            self.end_headers()
            self.wfile.write(b"x" * 10)
            self.close_connection = True
        elif p == "/slow":
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"y" * 10)
            self.wfile.flush()
            time.sleep(4)
            self.close_connection = True
        else:
            self.reply(404, b"?")

    def do_POST(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            data = b""
            while True:
                size = int(self.rfile.readline().split(b";")[0], 16)
                if not size:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    break
                data += self.rfile.read(size)
                self.rfile.readline()
            how = "chunked"
        else:
            data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            how = "length"
        if self.path == "/whoami":
            self.reply(200, str(self.client_address[1]).encode())
            return
        self.reply(200, f"{how} {len(data)} {hashlib.md5(data).hexdigest()}".encode())

    do_PUT = do_POST


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128                    # forty connections at once

    def handle_error(self, request, client_address):
        pass                                    # a client that hangs up after a refused certificate


class Brief(Handler):
    timeout = 0.3                               # this server drops a connection idle for 0.3 s


def raw_server(second):
    """A server on a bare socket: the first request on each connection gets "rude", the
    second gets `second` and then the connection is closed (b"": closed without a word).
    Returns its port and its counts."""
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    seen = {"conns": 0, "requests": 0}

    def conn(c):
        f = c.makefile("rb")
        n = 0
        while f.readline():
            while f.readline() not in (b"\r\n", b"\n", b""):
                pass
            with LOCK:
                seen["requests"] += 1
            n += 1
            if n == 2:
                c.sendall(second)
                break
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nrude\n")
        f.close()
        c.close()

    def loop():
        while True:
            c, _ = srv.accept()
            with LOCK:
                seen["conns"] += 1
            threading.Thread(target=conn, args=(c,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return srv.getsockname()[1], seen


def serve(context=None, handler=Handler):
    httpd = Server(("127.0.0.1", 0), handler)
    httpd.daemon_threads = True
    if context:
        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd.server_address[1]


def open_fetch(text, flags=0, length=0, timeout=0, body=None):
    st, handle, _, _ = wf.call(OPEN, 0, [flags, length, timeout, 0], 0, [text.encode("latin-1"), body, None, None], [0] * 4)
    return st, handle


def head(handle, wait=1000, room=65536):
    """(status, result, aux, the head parsed or None)."""
    st, res, aux, w = wf.call(HEAD, handle, [wait, 0, 0, 0], 1, [None] * 4, [room, 0, 0, 0])
    b = w.get(0, b"")
    if st or len(b) < 32:
        return st, res, aux, None
    magic, err, status, flags, length, tl, il, ver = struct.unpack(">8I", b[:32])
    assert magic == 0x4f574631 and len(b) == 32 + tl + il, (magic, len(b), tl, il)
    return st, res, aux, dict(error=err, status=status, flags=flags, length=length, version=ver,
                              text=b[32:32 + tl].decode("latin-1"), info=b[32 + tl:].decode("latin-1"))


def wait_head(handle, limit=15):
    t0 = time.time()
    while time.time() - t0 < limit:
        st, res, aux, h = head(handle)
        assert st == 0, st
        if h:
            assert (res, aux) == (h["status"], h["error"]), (res, aux, h)
            return h
    raise AssertionError("no head within %d s" % limit)


def read_all(handle, room=256 << 10, wait=1000):
    """(the body, the error at its end)."""
    out = []
    while True:
        st, n, aux, w = wf.call(READ, handle, [wait, 0, 0, 0], 1, [None] * 4, [room, 0, 0, 0])
        assert st == 0 and n == len(w[0]), (st, n)
        out.append(w[0])
        if aux & 1:
            return b"".join(out), aux >> 8


def close(handle):
    assert wf.call(CLOSE, handle, [0] * 4, 0, [None] * 4, [0] * 4)[0] == 0


def fetch(url, method="GET", headers="", flags=0, timeout=0):
    st, h = open_fetch(f"{method} {url}\n{headers}", flags, timeout=timeout)
    assert st == 0 and h, (url, st)
    hd = wait_head(h, limit=timeout + 15 if timeout else 45)
    body, err = read_all(h)
    close(h)
    return hd, body, err


def stats():
    _, n, threads, _ = wf.call(STATS, 0, [0] * 4, 0, [None] * 4, [0] * 4)
    return n, threads


def settle(limit=10):
    t0 = time.time()
    while time.time() - t0 < limit and stats() != (0, 0):
        time.sleep(0.05)
    return stats()


port = serve()
base = f"http://127.0.0.1:{port}"

# ---- plain HTTP --------------------------------------------------------------------------
hd, body, err = fetch(base + "/hello")
assert hd["status"] == 200 and hd["error"] == 0 and body == b"hello, Amiga\n" and err == 0, hd
assert hd["flags"] == H_LENGTH and hd["length"] == 13 and hd["version"] in (10, 11), hd
assert hd["text"].startswith("HTTP/1.") and "Set-Cookie: a=1; Path=/\nSet-Cookie: b=2\n" in hd["text"], hd["text"]
assert "X-Test: yes\n" in hd["text"] and hd["info"] == "", hd
print("GET: 200, both Set-Cookie headers back in order, length known, body right")

hd, body, _ = fetch(base + "/headers", headers="Cookie: session=42; theme=dark\r\nX-Amiga: 1200\nConnection: close\nHost: elsewhere\n")
sent = body.decode()
assert "Cookie: session=42; theme=dark\n" in sent and "X-Amiga: 1200\n" in sent, sent
assert f"Host: 127.0.0.1:{port}\n" in sent and "Connection: keep-alive\n" in sent and "User-Agent: GFOpenWeb-Fetch/1\n" in sent, sent
assert "elsewhere" not in sent and "close" not in sent, sent
hd, body, _ = fetch(base + "/headers", headers="User-Agent: OWFetch/0.1\n")
assert "User-Agent: OWFetch/0.1\n" in body.decode() and "GFOpenWeb-Fetch" not in body.decode()
print("request headers: Cookie and others as given; Host, Connection and User-Agent from the host")

hd, body, _ = fetch(base + "/redirect")
assert hd["status"] == 302 and "Location: /hello\n" in hd["text"] and body == b"", hd
hd, body, _ = fetch(base + "/missing")
assert hd["status"] == 404 and hd["error"] == 0 and body == b"no such page\n", hd
hd, body, _ = fetch(base + "/empty")
assert hd["status"] == 204 and hd["flags"] & H_NOBODY and body == b"", hd
hd, body, _ = fetch(base + "/big", method="HEAD")
assert hd["status"] == 200 and hd["flags"] & H_NOBODY and body == b"" and "Content-Length: %d\n" % len(BIG) in hd["text"], hd
print("302 comes back with its Location, not followed; 404 is a response; 204 and HEAD have no body")

t0 = time.time()
hd, body, err = fetch(base + "/big")
dt = time.time() - t0
assert hd["length"] == len(BIG) and hashlib.md5(body).hexdigest() == hashlib.md5(BIG).hexdigest() and err == 0
print(f"6 MB: MD5 matches, {len(BIG) / dt / 1e6:.0f} MB/s through 256 KB reads")

hd, body, _ = fetch(base + "/chunked")
assert body == TEXT and not hd["flags"] & H_LENGTH and "Transfer-Encoding" not in hd["text"], hd
hd, body, _ = fetch(base + "/close")
assert body == TEXT and not hd["flags"] & H_LENGTH, hd
print("chunked (with extensions and trailers) and close-delimited bodies come whole, de-chunked")

hd, body, _ = fetch(base + "/gzip", flags=DECODE)
assert body == TEXT and hd["flags"] & H_DECODED and "Content-Encoding" not in hd["text"], hd
assert not hd["flags"] & H_LENGTH or hd["length"] == len(TEXT), hd
hd, body, _ = fetch(base + "/gzip", headers="Accept-Encoding: gzip\n")
assert gzip.decompress(body) == TEXT and "Content-Encoding: gzip\n" in hd["text"] and hd["length"] == len(body), hd
hd, body, _ = fetch(base + "/deflate", flags=DECODE)
assert body == TEXT and hd["flags"] & H_DECODED, hd
st, h = open_fetch(f"GET {base}/gzip\n", DECODE)
hd = wait_head(h)
body, _ = read_all(h)
hd = head(h)[3]
assert body == TEXT and hd["flags"] & H_LENGTH and hd["length"] == len(TEXT) and f"Content-Length: {len(TEXT)}\n" in hd["text"], hd
close(h)
print(f"gzip and raw deflate undone with DECODE ({len(gzip.compress(TEXT))} -> {len(TEXT)} bytes), "
      "Content-Encoding dropped, the length fixed once the body is in; left alone without DECODE")
asked = fetch(base + "/headers", flags=DECODE)[1].decode()
if "Accept-Encoding: gzip, deflate, br\n" in asked and BR:
    hd, body, _ = fetch(base + "/br", flags=DECODE)
    assert body == TEXT and hd["flags"] & H_DECODED and "Content-Encoding" not in hd["text"], hd
    hd, body, _ = fetch(base + "/br", headers="Accept-Encoding: br\n")
    assert body == BR and "Content-Encoding: br\n" in hd["text"], hd
    hd, body, err = fetch(base + "/badbr", flags=DECODE)
    assert hd["status"] == 200 and err == E_DECODE, (hd, err)
    print(f"Brotli undone with DECODE ({len(BR)} -> {len(TEXT)} bytes), left alone without; bad Brotli is error 8")
else:
    print("Brotli: skipped (" + ("the library was built without libbrotlidec" if BR else "no libbrotlienc to make a body") + ")")

# ---- uploads -------------------------------------------------------------------------------
up = BIG[:3 << 20]
st, h = open_fetch(f"POST {base}/upload\nContent-Type: application/octet-stream\n", BODY, len(up), body=up[:1000])
assert st == 0
at = 1000
while at < len(up):
    piece = up[at:at + (64 << 10)]
    last = LAST if at + len(piece) == len(up) else 0
    st, n, aux, _ = wf.call(SEND, h, [last, 1000, 0, 0], 0, [piece, None, None, None], [0] * 4)
    assert st == 0 and aux == 0, (st, aux)
    at += n
hd = wait_head(h)
body, _ = read_all(h)
close(h)
assert body.decode() == f"length {len(up)} {hashlib.md5(up).hexdigest()}", body
st, h = open_fetch(f"PUT {base}/upload\n", BODY, UNKNOWN)
for i in range(0, len(TEXT), 50000):
    piece = TEXT[i:i + 50000]
    last = LAST if i + 50000 >= len(TEXT) else 0
    assert wf.call(SEND, h, [last, 1000, 0, 0], 0, [piece, None, None, None], [0] * 4)[:2] == (0, len(piece))
hd = wait_head(h)
body, _ = read_all(h)
assert body.decode() == f"chunked {len(TEXT)} {hashlib.md5(TEXT).hexdigest()}", body
assert wf.call(SEND, h, [LAST, 0, 0, 0], 0, [b"x", None, None, None], [0] * 4)[0] in (0, -2)
close(h)
st, h = open_fetch(f"POST {base}/upload\n", BODY, 5, body=b"abcde")
hd = wait_head(h)
assert read_all(h)[0].decode() == f"length 5 {hashlib.md5(b'abcde').hexdigest()}"
close(h)
print("uploads: 3 MB with a length (OPEN then SEND), chunked with no length, and all in OPEN; MD5s match")

# ---- bad requests --------------------------------------------------------------------------
for text in ("GET ftp://127.0.0.1/\n", "GET http://\n", "GET http://a b/\n", "GET http://user@host/\n",
             "G(T http://127.0.0.1/\n", "GET http://127.0.0.1:99999/\n", f"GET {base}/\nBad Header\n",
             f"GET {base}/\nX: a\x01b\n", "", "GET\n"):
    assert open_fetch(text)[0] == -2, repr(text)
assert open_fetch(f"GET {base}/hello\n", body=b"x")[0] == -2          # a body without WF_BODY
assert open_fetch(f"GET {base}/hello\n", 4)[0] == -2                   # an unknown flag
assert open_fetch(f"POST {base}/upload\n", BODY, 2, body=b"abc")[0] == -2
assert wf.call(READ, 12345, [0] * 4, 1, [None] * 4, [16, 0, 0, 0])[0] == -2
assert wf.call(99, 0, [0] * 4, 0, [None] * 4, [0] * 4)[0] == -2
st, h = open_fetch(f"GET {base}/hello\n")
wait_head(h)
st, need, _, _ = wf.call(HEAD, h, [0] * 4, 1, [None] * 4, [40, 0, 0, 0])
assert st == -4 and need > 40, (st, need)
assert head(h, room=need)[3]["status"] == 200
close(h)
assert wf.call(READ, h, [0] * 4, 1, [None] * 4, [16, 0, 0, 0])[0] == -2 and wf.call(CLOSE, h, [0] * 4, 0, [None] * 4, [0] * 4)[0] == -2
print("bad requests refused with -2; a small head buffer gets -4 and the size; a closed handle is gone")

# ---- errors ---------------------------------------------------------------------------------
hd, _, err = fetch("http://no-such-host.invalid/")
assert hd["error"] == E_DNS and hd["status"] == 0 and err == E_DNS and "error=" in hd["info"], hd
s = socket.socket()
s.bind(("127.0.0.1", 0))
closed_port = s.getsockname()[1]
s.close()
hd, _, _ = fetch(f"http://127.0.0.1:{closed_port}/")
assert hd["error"] == E_CONNECT and "refused" in hd["info"], hd
mute = socket.socket()                                                  # takes connections, never answers
mute.bind(("127.0.0.1", 0))
mute.listen(100)
mute_url = f"http://127.0.0.1:{mute.getsockname()[1]}/"
t0 = time.time()
hd, _, _ = fetch(mute_url, timeout=1)
assert hd["error"] == E_TIMEOUT and 0.9 < time.time() - t0 < 4, (hd, time.time() - t0)
hd, body, err = fetch(base + "/short")
assert hd["status"] == 200 and body == b"x" * 10 and err == E_CLOSED, (hd, err)
hd, body, err = fetch(base + "/badgzip", flags=DECODE)
assert hd["status"] == 200 and err == E_DECODE, (hd, err)
print("errors apart: DNS, connection refused, timeout after 1 s, closed early, bad gzip")

st, h = open_fetch(f"GET {base}/slow\n")
hd = wait_head(h)
assert wf.call(READ, h, [1000, 0, 0, 0], 1, [None] * 4, [64, 0, 0, 0])[1] == 10
got = {}


def waiting_read():
    st, n, aux, _ = wf.call(READ, h, [2000, 0, 0, 0], 1, [None] * 4, [64, 0, 0, 0])
    got.update(st=st, n=n, aux=aux, end=time.time())


r = threading.Thread(target=waiting_read)
r.start()
time.sleep(0.3)
t_cancel = time.time()
assert wf.call(CANCEL, h, [0] * 4, 0, [None] * 4, [0] * 4)[0] == 0
r.join()
got["t"] = got["end"] - t_cancel
assert got["st"] == 0 and got["aux"] == (1 | E_CANCELLED << 8) and got["t"] < 0.5, got
close(h)
st, h = open_fetch(f"GET {mute_url}\n")
time.sleep(0.2)
assert wf.call(CANCEL, h, [0] * 4, 0, [None] * 4, [0] * 4)[0] == 0
hd = wait_head(h)
assert hd["error"] == E_CANCELLED, hd
close(h)
print(f"cancel: a READ waiting on a slow body ends {got['t'] * 1000:.1f} ms after CANCEL; a fetch waiting for its head stops")

# ---- many at once, and the limit ---------------------------------------------------------------
results = {}


def one(i):
    hd, body, err = fetch(base + "/big")
    results[i] = hashlib.md5(body).hexdigest() == hashlib.md5(BIG).hexdigest() and err == 0 and hd["status"] == 200


t0 = time.time()
ts = [threading.Thread(target=one, args=(i,)) for i in range(8)]
for t in ts:
    t.start()
for t in ts:
    t.join()
assert len(results) == 8 and all(results.values()), results
print(f"8 fetches of 6 MB at once: all MD5s match, {8 * len(BIG) / (time.time() - t0) / 1e6:.0f} MB/s together")

handles = []
for i in range(64):
    st, h = open_fetch(f"GET {mute_url}\n")
    assert st == 0, (i, st)
    handles.append(h)
assert open_fetch(f"GET {mute_url}\n")[0] == -5 and stats()[0] == 64
for h in handles:
    close(h)
assert settle() == (0, 0), stats()
print("64 fetches open at once; the 65th gets -5; after CLOSE no fetch or thread is left")

# ---- answering later: HEAD and READ through wf_start, as the services card calls ------------------
card = wf.Card()


def parse_head(b):
    magic, err, status, flags, length, tl, il, ver = struct.unpack(">8I", b[:32])
    assert magic == 0x4f574631, magic
    return dict(error=err, status=status, flags=flags, text=b[32:32 + tl].decode("latin-1"),
                info=b[32 + tl:32 + tl + il].decode("latin-1"))


st, h = open_fetch(f"GET {base}/slowhead\n")
t0 = time.time()
i, st, _, _, _ = card.start(HEAD, h, 2000, 4096)
assert st == wf.LATER, st
a = card.wait(i)
dt = a[4] - t0
assert a[0] == 0 and a[1] == 200 and a[2] == 0 and parse_head(a[3])["status"] == 200 and 0.4 < dt < 1.5, (a[:3], dt)
assert read_all(h)[0] == b"slow head\n"
close(h)
time.sleep(0.05)
assert card.count(i) == 1
print(f"HEAD held: answered once from the library's thread when the head came, {dt * 1000:.0f} ms later")

st, h = open_fetch(f"GET {base}/slow\n")
wait_head(h)
assert wf.call(READ, h, [1000, 0, 0, 0], 1, [None] * 4, [64, 0, 0, 0])[1] == 10
i, st, n, aux, data = card.start(READ, h, 0, 64)
assert (st, n, aux, data) == (0, 0, 0, b""), (st, n, aux)            # no wait: answered at once, nothing yet
t0 = time.time()
i, st, _, _, _ = card.start(READ, h, 300, 64)
assert st == wf.LATER, st
a = card.wait(i)
dt = a[4] - t0
assert a[:3] == (0, 0, 0) and 0.25 < dt < 1.0, (a[:3], dt)            # the wait ran out: nothing yet, more to come
i, st, _, _, _ = card.start(READ, h, 2000, 64)
assert st == wf.LATER
time.sleep(0.1)
t0 = time.time()
card.cancel(i)
a = card.wait(i)
cancel_ms = (a[4] - t0) * 1000
assert a[0] == -3 and cancel_ms < 50, (a[:3], cancel_ms)
card.cancel(i)                                                         # again, and an id never given: nothing happens
card.cancel(999999)
i2, st, _, _, _ = card.start(READ, h, 2000, 64)
i3, st3, _, _, _ = card.start(READ, h, 2000, 64)
assert st == st3 == wf.LATER
time.sleep(0.1)
t0 = time.time()
assert wf.call(CANCEL, h, [0] * 4, 0, [None] * 4, [0] * 4)[0] == 0
a2, a3 = card.wait(i2), card.wait(i3)
assert a2[:3] == a3[:3] == (0, 0, 1 | E_CANCELLED << 8) and a3[4] - t0 < 0.1, (a2[:3], a3[:3])
close(h)
time.sleep(0.05)
assert card.count(i) == card.count(i2) == card.count(i3) == 1
print(f"READ held: its wait runs out after {dt * 1000:.0f} ms with 'not yet'; a card cancel answers -3 in "
      f"{cancel_ms:.2f} ms, and only once; the fetch's CANCEL ends two held READs with error 6")

st, h = open_fetch(f"GET {mute_url}\n")
i, st, _, _, _ = card.start(HEAD, h, 2000, 4096)
assert st == wf.LATER
time.sleep(0.1)
close(h)
a = card.wait(i, 1)
assert a and a[0] == 0 and a[2] == E_CANCELLED and parse_head(a[3])["error"] == E_CANCELLED, a and a[:3]
print("CLOSE answers a held HEAD at once, with error 6")

t0 = time.time()
hs = [open_fetch(f"GET {base}/slowhead\n")[1] for _ in range(40)]
held = [card.start(HEAD, h, 2000, 4096) for h in hs]
assert all(x[1] == wf.LATER for x in held), [x[1] for x in held]
ids, status, again = [x[0] for x in held], [], 0
for h, i in zip(hs, list(ids)):
    a = card.wait(i)
    while a[:3] == (0, 0, 0):                                           # the wait ran out first: ask again, as a client does
        again += 1
        i, st, res, aux, _ = card.start(HEAD, h, 2000, 4096)
        ids.append(i)
        a = card.wait(i) if st == wf.LATER else (st, res, aux)
    status.append(a[:2])
dt = time.time() - t0
assert status == [(0, 200)] * 40 and dt < 6, (dt, status)
for h in hs:
    assert read_all(h)[0] == b"slow head\n"
    close(h)
time.sleep(0.05)
assert all(card.count(i) <= 1 for i in ids) and all(card.count(x[0]) == 1 for x in held)
assert settle() == (0, 0), stats()
print(f"40 HEADs held at once, one on each of 40 fetches: each answered once, all 200 in {dt:.2f} s "
      f"(each head takes 0.5 s; {again} asked again after a 2 s wait ran out)")

# ---- kept connections -----------------------------------------------------------------------------


def whoami(path="/whoami", where=None):
    hd, body, err = fetch((where or base) + path)
    assert hd["status"] == 200 and err == 0, (hd, err)
    return int(body), hd


def closed_soon(port, limit=3.0):
    """Seconds until the server saw that connection close, or None."""
    t0 = time.time()
    while time.time() - t0 < limit:
        with LOCK:
            if port in CLOSED:
                return time.time() - t0
        time.sleep(0.01)
    return None


a, hd1 = whoami()
b, hd2 = whoami()
assert a == b and "reused=yes\n" in hd2["info"], (a, b, hd2["info"])
assert {whoami()[0] for _ in range(30)} == {a}
print("kept: a second fetch to the same host and port goes on the same connection (info says reused=yes), "
      "and 30 more after it")

st, h = open_fetch(f"POST {base}/whoami\n", BODY, 3, body=b"abc")
wait_head(h)
p = int(read_all(h)[0])
close(h)
assert p != a and whoami()[0] == p, (a, p)
print("a request with a body opens its own connection (it could not be sent again); that connection is kept after it")

c, _ = whoami("/closing")
assert closed_soon(c, 1) is not None and whoami("/closing")[0] != c
print("an answer with Connection: close: the connection is not kept")

p, _ = whoami("/ka2")
time.sleep(0.2)
q, _ = whoami("/ka2")
waited = closed_soon(q)
assert q == p and waited is not None and 0.6 < waited < 2.0, (p, q, waited)
print(f"Keep-Alive: timeout=2 from the server: the host closed the idle connection {waited:.1f} s after its last answer")

with LOCK:
    c0 = len(CLOSED)
st, h = open_fetch(f"GET {base}/big\n")
wait_head(h)
close(h)                                                                # 6 MB not read
t0 = time.time()
while time.time() - t0 < 2 and len(CLOSED) == c0:
    time.sleep(0.01)
assert len(CLOSED) == c0 + 1, (c0, len(CLOSED))
print("a fetch closed before its body ended: its connection is closed, not kept")

with LOCK:
    c0 = len(CLOSED)
ports = []
ts = [threading.Thread(target=lambda: ports.append(whoami("/slowwho")[0])) for _ in range(10)]
for t in ts:
    t.start()
for t in ts:
    t.join()
time.sleep(0.3)
with LOCK:
    gone = set(CLOSED[c0:])
assert len(set(ports)) == 10 and len(gone & set(ports)) == 4, (ports, gone)
print("10 fetches at once to one host: 10 connections; 6 kept after, 4 closed")

brief = serve(handler=Brief)
x, _ = whoami(where=f"http://127.0.0.1:{brief}")
assert closed_soon(x) is not None                                      # the server dropped it after 0.3 s idle
y, hd = whoami(where=f"http://127.0.0.1:{brief}")
assert y != x and "reused" not in hd["info"], (x, y, hd["info"])
print("a kept connection the server closed while idle is found closed before use; the fetch opens a new one")

rude, seen = raw_server(b"")
hd1, body1, err1 = fetch(f"http://127.0.0.1:{rude}/")
hd2, body2, err2 = fetch(f"http://127.0.0.1:{rude}/")
assert body1 == body2 == b"rude\n" and err1 == err2 == 0 and hd2["status"] == 200, (body1, body2, err1, err2)
assert seen == {"conns": 2, "requests": 3} and "reused" not in hd2["info"], (seen, hd2["info"])
print("a kept connection the server closes as the next request comes: the request goes again on a new one, once")

cut, seen = raw_server(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort")
fetch(f"http://127.0.0.1:{cut}/")
hd, body, err = fetch(f"http://127.0.0.1:{cut}/")
assert hd["status"] == 200 and body == b"short" and err == E_CLOSED and seen == {"conns": 1, "requests": 2}, (hd, body, err, seen)
print("a kept connection closed in the middle of an answer: error 7 (closed early), not sent again")
assert settle() == (0, 0), stats()

# ---- HTTPS -----------------------------------------------------------------------------------
CHILD = r"""
import sys, struct, time
sys.path.insert(0, sys.argv[1])
import openweb_fetch as wf
for url in sys.argv[2:]:
    _, h, _, _ = wf.call(1, 0, [0, 0, 10, 0], 0, [("GET %s\n" % url).encode(), None, None, None], [0] * 4)
    t0 = time.time()
    while time.time() - t0 < 15:
        st, res, aux, w = wf.call(3, h, [1000, 0, 0, 0], 1, [None] * 4, [65536, 0, 0, 0])
        if w.get(0):
            break
    b = w[0]
    err, status, flags, length, tl, il = struct.unpack(">6I", b[4:28])
    print(url, err, status, flags, b[32 + tl:].decode().replace("\n", "|"))
    wf.call(6, h, [0] * 4, 0, [None] * 4, [0] * 4)
"""

PAGE = r"""
import sys, struct, time
sys.path.insert(0, sys.argv[1])
import openweb_fetch as wf
url, n, ports, info = sys.argv[2], int(sys.argv[3]), set(), ""
t0 = time.time()
for _ in range(n):
    _, h, _, _ = wf.call(1, 0, [0, 0, 10, 0], 0, [("GET %s\n" % url).encode(), None, None, None], [0] * 4)
    while True:
        st, res, aux, w = wf.call(3, h, [1000, 0, 0, 0], 1, [None] * 4, [65536, 0, 0, 0])
        if w.get(0):
            break
    tl, il = struct.unpack(">2I", w[0][20:28])
    info = w[0][32 + tl:32 + tl + il].decode().replace("\n", "|")
    body = b""
    while True:
        st, k, aux, w = wf.call(4, h, [1000, 0, 0, 0], 1, [None] * 4, [65536, 0, 0, 0])
        body += w[0]
        if aux & 1:
            break
    ports.add(body)
    wf.call(6, h, [0] * 4, 0, [None] * 4, [0] * 4)
print("%.1f %d %s" % ((time.time() - t0) * 1000, len(ports), info))
"""

with tempfile.TemporaryDirectory() as tmp:
    def make_cert(name, sans):
        key, cert = os.path.join(tmp, name + ".key"), os.path.join(tmp, name + ".pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                        "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=" + name,
                        "-addext", "subjectAltName=" + sans], check=True, capture_output=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        return cert, serve(ctx)

    cert, tls_port = make_cert("localhost", "DNS:localhost,IP:127.0.0.1")
    other, other_port = make_cert("other.test", "DNS:other.test")
    hd, _, err = fetch(f"https://127.0.0.1:{tls_port}/hello")
    assert hd["error"] == E_TLS and hd["status"] == 0 and err == E_TLS, hd
    assert ("self-signed" in hd["info"] or "self signed" in hd["info"]) and "subject=CN=localhost\n" in hd["info"], hd["info"]
    assert "expires=" in hd["info"], hd["info"]
    print("HTTPS with a certificate the system does not trust: error 3,", hd["info"].split("\n")[0])
    with open(os.path.join(tmp, "trust.pem"), "wb") as f:
        f.write(open(cert, "rb").read() + open(other, "rb").read())
    out = subprocess.run([sys.executable, "-c", CHILD, os.path.dirname(os.path.abspath(wf.__file__)),
                          f"https://localhost:{tls_port}/hello", f"https://127.0.0.1:{tls_port}/hello",
                          f"https://localhost:{other_port}/hello"],
                         env=dict(os.environ, SSL_CERT_FILE=os.path.join(tmp, "trust.pem")),
                         capture_output=True, text=True, timeout=120).stdout.splitlines()
    good, ip, wrong = (line.split(" ", 4) for line in out)
    assert good[1:4] == ["0", "200", str(H_TLS | H_VERIFIED | H_LENGTH)] and "verified=yes" in good[4], good
    assert "tls=TLSv1.3" in good[4] and "subject=CN=localhost" in good[4], good
    assert ip[1:3] == ["0", "200"], ip
    assert wrong[1:3] == [str(E_TLS), "0"] and "mismatch" in wrong[4] and "subject=CN=other.test" in wrong[4], wrong
    print("HTTPS trusted through SSL_CERT_FILE (a child process): 200 by name and by IP,",
          good[4].split("|")[0] + "; a name the certificate lacks: " + wrong[4].split("|")[0])

    def page(url, keep, n=50):
        """n small fetches one after another in a child process: (ms, connections, the last info)."""
        env = dict(os.environ, SSL_CERT_FILE=os.path.join(tmp, "trust.pem"))
        if not keep:
            env["OPENWEB_FETCH_KEEP"] = "0"
        out = subprocess.run([sys.executable, "-c", PAGE, os.path.dirname(os.path.abspath(wf.__file__)), url, str(n)],
                             env=env, capture_output=True, text=True, timeout=120)
        ms, conns, info = (out.stdout.strip() + "  ").split(" ", 2)
        return float(ms), int(conns), info.strip()

    tls_kept = page(f"https://localhost:{tls_port}/whoami", True)
    tls_fresh = page(f"https://localhost:{tls_port}/whoami", False)
    http_kept = page(f"{base}/whoami", True)
    http_fresh = page(f"{base}/whoami", False)
    assert tls_kept[1] == http_kept[1] == 1 and tls_fresh[1] == http_fresh[1] == 50, (tls_kept, tls_fresh, http_kept, http_fresh)
    assert "reused=yes" in tls_kept[2] and "tls=TLSv1.3" in tls_kept[2] and "verified=yes" in tls_kept[2], tls_kept[2]
    assert "reused" not in tls_fresh[2] and tls_kept[0] < tls_fresh[0], (tls_kept, tls_fresh)
    print(f"a page of 50 small fetches, one after another: HTTPS {tls_kept[0]:.0f} ms on 1 kept connection "
          f"(each with its TLS summary and reused=yes) against {tls_fresh[0]:.0f} ms on 50 (OPENWEB_FETCH_KEEP=0); "
          f"HTTP {http_kept[0]:.0f} ms against {http_fresh[0]:.0f} ms")

assert settle() == (0, 0), stats()
print("nothing left open")

# ---- the real network --------------------------------------------------------------------------
try:
    socket.create_connection(("example.com", 443), timeout=5).close()
    online = True
except OSError:
    online = False
if online:
    hd, body, err = fetch("https://example.com/", headers="Accept: text/html\n", flags=DECODE)
    assert hd["status"] == 200 and hd["flags"] & H_VERIFIED and b"Example Domain" in body and err == 0, hd
    print("https://example.com/: 200,", len(body), "bytes,", "; ".join(l for l in hd["info"].splitlines() if l.startswith(("tls=", "issuer="))))
else:
    print("https://example.com/: skipped, no network from here")
