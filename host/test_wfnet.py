"""Checks the native transport of openweb.fetch (src/wfnet*.c, the Amiga's own protocol code, built for
Linux as host/libwfnet.so with host/wfnet_posix.c) against servers this test starts on 127.0.0.1:
HTTP/1.1 (kept connections, chunked, uploads, timeouts, cancel), an HTTP/2 server over TLS written
here by hand (ALPN h2, HPACK both ways, streams side by side, flow control, RST_STREAM, GOAWAY), a
WebSocket echo server, an HTTP proxy with CONNECT, and Basic and Digest auth. First the pieces with
published examples: RFC 7541 Appendix C, RFC 1321, FIPS 180-4, RFC 4648, RFC 6455 and RFC 7616.
MIT, Copyright (c) 2026 AmigaChrome Team.
SPDX-License-Identifier: MIT"""
import base64
import ctypes
import gzip
import hashlib
import http.server
import os
import random
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = tempfile.mkdtemp(prefix="wfnet-")

# The test's own certificate authority and a certificate for localhost and 127.0.0.1, trusted
# through SSL_CERT_FILE, which OpenSSL reads when the library's context is made.
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=wfnet test CA",
                "-keyout", f"{TMP}/ca.key", "-out", f"{TMP}/ca.pem", "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign"], check=True, capture_output=True)
with open(f"{TMP}/ext.cnf", "w") as f:
    f.write("subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n")
subprocess.run(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost", "-keyout", f"{TMP}/srv.key",
                "-out", f"{TMP}/srv.csr"], check=True, capture_output=True)
subprocess.run(["openssl", "x509", "-req", "-in", f"{TMP}/srv.csr", "-CA", f"{TMP}/ca.pem", "-CAkey", f"{TMP}/ca.key",
                "-CAcreateserial", "-days", "2", "-extfile", f"{TMP}/ext.cnf", "-out", f"{TMP}/srv.pem"],
               check=True, capture_output=True)
os.environ["SSL_CERT_FILE"] = f"{TMP}/ca.pem"
for k in ("http_proxy", "https_proxy", "all_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY"):
    os.environ.pop(k, None)

LIB = ctypes.CDLL(os.path.join(HERE, "libwfnet.so"))


class Buf(ctypes.Structure):
    _fields_ = [("inp", ctypes.c_void_p), ("out", ctypes.c_void_p), ("length", ctypes.c_ulong), ("written", ctypes.c_ulong)]


UL = ctypes.c_ulong
LIB.wfnp_new.restype = ctypes.c_void_p
LIB.wfnp_new.argtypes = [UL, ctypes.c_int]
LIB.wfnp_call.restype = ctypes.c_long
LIB.wfnp_call.argtypes = [ctypes.c_void_p, ctypes.c_uint, UL, ctypes.POINTER(UL), ctypes.POINTER(Buf), ctypes.POINTER(UL), ctypes.POINTER(UL)]
LIB.wfnp_free.argtypes = [ctypes.c_void_p]

OPEN, SEND, HEAD, READ, CANCEL, CLOSE, STATS, WS_OPEN, WS_SEND, WS_RECV = range(1, 11)
BODY, DECODE, NOPROXY, HTTP1 = 1, 2, 4, 8
UNKNOWN = 0xffffffff
H_TLS, H_VERIFIED, H_DECODED, H_LENGTH, H_NOBODY = 1, 2, 4, 8, 16
E_DNS, E_CONNECT, E_TLS, E_TIMEOUT, E_PROTOCOL, E_CANCELLED, E_CLOSED, E_DECODE = range(1, 9)
WS_TEXT, WS_BINARY, WS_CLOSE, WS_PING, WS_PONG = 1, 2, 8, 9, 10
WS_FIN, WS_END, WS_MORE = 0x10, 0x20, 0x80


class Fetcher:
    """One struct wfn: a task's fetches, all moved on by any call."""

    def __init__(self, window=65536, decoders=False, lib=None):
        self.lib = lib or LIB
        self.w = self.lib.wfnp_new(window, 1 if decoders else 0)
        assert self.w

    def call(self, op, arg=0, extra=(0, 0, 0, 0), inp=(), room=0):
        """inp: bytes for buffers 0, 1; room: an out buffer 0. (status, result, aux, bytes written)."""
        bufs = (Buf * 4)()
        keep = []
        for i, data in enumerate(inp):
            if data is not None:
                b = ctypes.create_string_buffer(bytes(data), len(data) or 1)
                keep.append(b)
                bufs[i].inp = ctypes.cast(b, ctypes.c_void_p)
                bufs[i].length = len(data)
        if room:
            out = ctypes.create_string_buffer(room)
            keep.append(out)
            bufs[0].out = ctypes.cast(out, ctypes.c_void_p)
            bufs[0].length = room
        ex = (UL * 4)(*extra)
        res, aux = UL(), UL()
        st = self.lib.wfnp_call(self.w, op, arg, ex, bufs, ctypes.byref(res), ctypes.byref(aux))
        return st, res.value, aux.value, (ctypes.string_at(bufs[0].out, bufs[0].written) if room else b"")

    def open(self, text, flags=0, length=0, timeout=0, body=None, ws=False):
        st, h, _, _ = self.call(WS_OPEN if ws else OPEN, 0, (flags, length, timeout, 0),
                                (text.encode() if isinstance(text, str) else text, body))
        assert st == 0, (st, text)
        return h

    def head(self, h, wait=1000, limit=20):
        end = time.time() + limit
        while time.time() < end:
            st, status, err, out = self.call(HEAD, h, (wait, 0, 0, 0), room=70000)
            assert st == 0, st
            if status or err:
                return parse_head(out)
        raise AssertionError("no head")

    def read_all(self, h, room=65536):
        data, end = b"", 0
        t = time.time() + 30
        while not end:
            st, n, end, out = self.call(READ, h, (1000, 0, 0, 0), room=room)
            assert st == 0 and time.time() < t, st
            data += out
        return data, end >> 8

    def close(self, h):
        assert self.call(CLOSE, h)[0] == 0

    def stats(self):
        return self.call(STATS)[1:3]

    def fetch(self, url, method="GET", headers="", flags=0, body=None, timeout=0, length=None):
        if body is not None:
            flags |= BODY
            length = len(body) if length is None else length
        h = self.open(f"{method} {url}\n{headers}", flags, length or 0, timeout, body)
        hd = self.head(h)
        data, err = self.read_all(h) if not hd["error"] else (b"", hd["error"])
        self.close(h)
        return hd, data, err

    def free(self):
        self.lib.wfnp_free(self.w)


def parse_head(b):
    magic, err, status, flags, length, tl, il, ver = struct.unpack(">8I", b[:32])
    assert magic == 0x4f574631
    text = b[32:32 + tl].decode("latin-1")
    info = b[32 + tl:32 + tl + il].decode("latin-1")
    headers = {}
    for line in text.splitlines()[1:]:
        k, _, v = line.partition(":")
        headers.setdefault(k.strip().lower(), []).append(v.strip())
    return {"error": err, "status": status, "flags": flags, "length": length, "text": text, "info": info,
            "version": ver, "headers": headers}


# ---- the pieces, from their published examples -----------------------------------------------------

def c_hash(name, data, n):
    out = ctypes.create_string_buffer(n)
    getattr(LIB, name)(data, UL(len(data)), out)
    return out.raw


for data in (b"", b"a", b"abc", b"message digest", b"x" * 55, b"y" * 56, b"z" * 64, b"q" * 1000, random.randbytes(5000)):
    assert c_hash("wfn_md5", data, 16) == hashlib.md5(data).digest()
    assert c_hash("wfn_sha1", data, 20) == hashlib.sha1(data).digest()
    assert c_hash("wfn_sha256", data, 32) == hashlib.sha256(data).digest()
assert c_hash("wfn_md5", b"abc", 16).hex() == "900150983cd24fb0d6963f7d28e17f72"            # RFC 1321 A.5
assert c_hash("wfn_sha1", b"abc", 20).hex() == "a9993e364706816aba3e25717850c26c9cd0d89d"     # FIPS 180 example
for data in (b"", b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar"):                          # RFC 4648 section 10
    out = ctypes.create_string_buffer(16)
    LIB.wfn_base64(data, UL(len(data)), out)
    assert out.value == base64.b64encode(data), data
out = ctypes.create_string_buffer(29)
LIB.wfn_ws_accept(b"dGhlIHNhbXBsZSBub25jZQ==", out)                                          # RFC 6455 section 1.3
assert out.value == b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", out.value
print("MD5, SHA-1, SHA-256 and base64 as hashlib and the RFCs; Sec-WebSocket-Accept as RFC 6455 1.3")

LIB.wfn_auth_answer.restype = UL
LIB.wfn_auth_answer.argtypes = [ctypes.c_char_p, UL, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                UL, ctypes.c_char_p, ctypes.c_char_p, UL]


def auth_answer(challenge, user, pw, method, uri, cnonce, nc=1):
    out = ctypes.create_string_buffer(2048)
    n = LIB.wfn_auth_answer(challenge.encode(), len(challenge), user.encode(), pw.encode(), method.encode(), uri.encode(),
                            nc, cnonce.encode(), out, 2048)
    return out.value.decode() if n else None


def digest_params(answer):
    assert answer.startswith("Digest ")
    out = {}
    for part in answer[7:].split(", "):
        k, _, v = part.partition("=")
        out[k] = v.strip('"')
    return out


# RFC 7616 section 3.9.1: the same request with MD5 and SHA-256.
NONCE, OPAQUE = "7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v", "FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS"
CNONCE = "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ"
for alg, want in (("MD5", "8ca523f5e9506fed4657c9700eebdbec"),
                  ("SHA-256", "753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1")):
    ch = f'Digest realm="http-auth@example.org", qop="auth, auth-int", algorithm={alg}, nonce="{NONCE}", opaque="{OPAQUE}"'
    p = digest_params(auth_answer(ch, "Mufasa", "Circle of Life", "GET", "/dir/index.html", CNONCE))
    assert p["response"] == want and p["algorithm"] == alg and p["qop"] == "auth" and p["nc"] == "00000001", p
    assert p["opaque"] == OPAQUE and p["uri"] == "/dir/index.html", p
# Several challenges in one value: SHA-256 Digest is taken over MD5 Digest and Basic.
two = (f'Basic realm="x", Digest realm="http-auth@example.org", qop="auth", algorithm=MD5, nonce="{NONCE}", '
       f'Digest realm="http-auth@example.org", qop="auth", algorithm=SHA-256, nonce="{NONCE}", opaque="{OPAQUE}"')
assert digest_params(auth_answer(two, "Mufasa", "Circle of Life", "GET", "/dir/index.html", CNONCE))["algorithm"] == "SHA-256"
assert auth_answer('Basic realm="WallyWorld"', "Aladdin", "open sesame", "GET", "/", "") == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="
assert auth_answer('Negotiate', "a", "b", "GET", "/", "") is None
print("Digest answers as RFC 7616 3.9.1 (MD5 and SHA-256); Basic as RFC 7617; the strongest challenge chosen")

# ---- HPACK: RFC 7541 Appendix C --------------------------------------------------------------------

LIB.wfn_hpack_new.restype = ctypes.c_void_p
LIB.wfn_hpack_new.argtypes = [ctypes.c_void_p, UL]
LIB.wfn_hpack_decode.restype = ctypes.c_long
LIB.wfn_hpack_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, UL, ctypes.c_char_p, UL]
LIB.wfn_hpack_encode.restype = UL
LIB.wfn_hpack_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, UL, ctypes.c_char_p, UL, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_char_p, UL]
LIB.wfn_hpack_table_size.restype = UL
LIB.wfn_hpack_table_size.argtypes = [ctypes.c_void_p]
LIB.wfn_huff_encode.restype = UL
LIB.wfn_huff_encode.argtypes = [ctypes.c_char_p, UL, ctypes.c_char_p, UL]
LIB.wfn_huff_decode.restype = ctypes.c_long
LIB.wfn_huff_decode.argtypes = [ctypes.c_char_p, UL, ctypes.c_char_p, UL]


class IO(ctypes.Structure):
    """Enough of struct wfn_io for wfn_hpack_new: env, alloc, release."""
    _fields_ = [("env", ctypes.c_void_p), ("alloc", ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, UL)),
                ("release", ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p))]


libc = ctypes.CDLL(None)
libc.calloc.restype = ctypes.c_void_p
libc.calloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
libc.free.argtypes = [ctypes.c_void_p]
IO_ = IO(None, IO._fields_[1][1](lambda e, n: libc.calloc(1, n)), IO._fields_[2][1](lambda e, p: libc.free(p)))


def hx(s):
    return bytes.fromhex(s.replace(" ", ""))


def decode_seq(blocks, table=4096):
    h = LIB.wfn_hpack_new(ctypes.byref(IO_), table)
    out = []
    for b in blocks:
        buf = ctypes.create_string_buffer(8192)
        n = LIB.wfn_hpack_decode(h, b, len(b), buf, 8192)
        assert n >= 0, b.hex()
        out.append((buf.raw[:n].decode(), LIB.wfn_hpack_table_size(h)))
    return out


def encode_seq(lists, huffman, table=4096):
    h = LIB.wfn_hpack_new(ctypes.byref(IO_), table)
    out = []
    for fields in lists:
        b = b""
        for name, value, mode in fields:
            buf = ctypes.create_string_buffer(512)
            n = LIB.wfn_hpack_encode(h, name.encode(), len(name), value.encode(), len(value), 1 if mode == "index" else 0,
                                     1 if huffman else 0, buf, 512)
            assert n, (name, value)
            b += buf.raw[:n]
        out.append(b)
    return out


def lines(*pairs):
    return "".join(f"{k}: {v}\n" for k, v in pairs)


# C.2: single fields
assert decode_seq([hx("400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572")]) == [(lines(("custom-key", "custom-header")), 55)]
assert decode_seq([hx("040c 2f73 616d 706c 652f 7061 7468")]) == [(lines((":path", "/sample/path")), 0)]
assert decode_seq([hx("1008 7061 7373 776f 7264 0673 6563 7265 74")]) == [(lines(("password", "secret")), 0)]
assert decode_seq([hx("82")]) == [(lines((":method", "GET")), 0)]
REQ = [[(":method", "GET", "x"), (":scheme", "http", "x"), (":path", "/", "x"), (":authority", "www.example.com", "index")],
       [(":method", "GET", "x"), (":scheme", "http", "x"), (":path", "/", "x"), (":authority", "www.example.com", "index"),
        ("cache-control", "no-cache", "index")],
       [(":method", "GET", "x"), (":scheme", "https", "x"), (":path", "/index.html", "x"),
        (":authority", "www.example.com", "index"), ("custom-key", "custom-value", "index")]]
C3 = [hx("8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d"), hx("8286 84be 5808 6e6f 2d63 6163 6865"),
      hx("8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65")]
C4 = [hx("8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff"), hx("8286 84be 5886 a8eb 1064 9cbf"),
      hx("8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf")]
want_req = [(lines(*[(n, v) for n, v, _ in r]), size) for r, size in zip(REQ, (57, 110, 164))]
assert decode_seq(C3) == want_req and decode_seq(C4) == want_req
assert encode_seq(REQ, False) == C3, [b.hex() for b in encode_seq(REQ, False)]
assert encode_seq(REQ, True) == C4, [b.hex() for b in encode_seq(REQ, True)]
# C.5 and C.6: responses, the table limited to 256 bytes, so entries are evicted.
DATE1, DATE2 = "Mon, 21 Oct 2013 20:13:21 GMT", "Mon, 21 Oct 2013 20:13:22 GMT"
COOKIE = "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"
RESP = [[(":status", "302", "index"), ("cache-control", "private", "index"), ("date", DATE1, "index"),
         ("location", "https://www.example.com", "index")],
        [(":status", "307", "index"), ("cache-control", "private", "index"), ("date", DATE1, "index"),
         ("location", "https://www.example.com", "index")],
        [(":status", "200", "index"), ("cache-control", "private", "index"), ("date", DATE2, "index"),
         ("location", "https://www.example.com", "index"), ("content-encoding", "gzip", "index"),
         ("set-cookie", COOKIE, "index")]]
C5 = [hx("4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3120 474d"
         "546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70 6c65 2e63 6f6d"),
      hx("4803 3330 37c1 c0bf"),
      hx("88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d 54c0 5a04 677a 6970 7738 666f"
         "6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49 553b 206d 6178 2d61 6765 3d33 3630 303b"
         "2076 6572 7369 6f6e 3d31")]
C6 = [hx("4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6 2d1b ff6e 919d 29ad 1718 63c7"
         "8f0b 97c8 e9ae 82ae 43d3"),
      hx("4883 640e ffc1 c0bf"),
      hx("88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab 77ad 94e7 821d d7f2 e6c7 b335"
         "dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed 4ee5 b106 3d50 07")]
want_resp = [(lines(*[(n, v) for n, v, _ in r]), size) for r, size in zip(RESP, (222, 222, 215))]
assert decode_seq(C5, 256) == want_resp, decode_seq(C5, 256)
assert decode_seq(C6, 256) == want_resp, decode_seq(C6, 256)
assert encode_seq(RESP, False, 256) == C5, [b.hex() for b in encode_seq(RESP, False, 256)]
assert encode_seq(RESP, True, 256) == C6, [b.hex() for b in encode_seq(RESP, True, 256)]


def huff(data):
    out = ctypes.create_string_buffer(len(data) * 4 + 8)
    n = LIB.wfn_huff_encode(data, len(data), out, len(data) * 4 + 8)
    back = ctypes.create_string_buffer(len(data) + 8)
    m = LIB.wfn_huff_decode(out.raw[:n], n, back, len(data) + 8)
    return out.raw[:n], back.raw[:m] if m >= 0 else None


every = bytes(range(256)) * 3
assert huff(every)[1] == every
for _ in range(200):
    s = random.randbytes(random.randrange(0, 60))
    assert huff(s)[1] == s
bad = ctypes.create_string_buffer(16)
assert LIB.wfn_huff_decode(b"\xff\xff\xff\xff", 4, bad, 16) == -1        # EOS inside a string
assert LIB.wfn_huff_decode(b"\x1f\x00", 2, bad, 16) == -1               # padding not all ones
print("HPACK as RFC 7541 Appendix C: C.2 fields, C.3/C.4 requests and C.5/C.6 responses (eviction), decoded and "
      "encoded byte for byte, plain and Huffman; Huffman round trips of every byte; EOS and bad padding refused")

# ---- servers ----------------------------------------------------------------------------------------

LOCK = threading.Lock()
SEEN = {"ports": [], "proxy": [], "h2conns": 0, "h2streams": [], "ws_pong": []}
TEXT = b"".join(b"Line %d of a page the Amiga asked for.\n" % i for i in range(3000))
BIG = random.Random(7).randbytes(3 << 20)
GZBIG = gzip.compress(BIG, 1)
CODED = {}                                             # path: (body, Content-Encoding), for the datatypes' tests
DIGEST_NONCE = "dcd98b7102dd2f0e8b11d0f600bfb0c093"


def digest_ok(header, method, alg):
    if not header or not header.startswith("Digest "):
        return False
    p = {}
    for part in header[7:].split(","):
        k, _, v = part.strip().partition("=")
        p[k] = v.strip('"')
    H = (lambda s: hashlib.sha256(s.encode()).hexdigest()) if alg == "SHA-256" else (lambda s: hashlib.md5(s.encode()).hexdigest())
    ha1 = H(f"{p.get('username')}:test@wfnet:{'pa ss'}")
    ha2 = H(f"{method}:{p.get('uri')}")
    want = H(f"{ha1}:{p.get('nonce')}:{p.get('nc')}:{p.get('cnonce')}:auth:{ha2}")
    return p.get("username") == "us er" and p.get("nonce") == DIGEST_NONCE and p.get("response") == want and p.get("opaque") == "op"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def reply(self, code, body, headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if code not in (204, 304):
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and code not in (204, 304):
            self.wfile.write(body)

    def body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            data = b""
            while True:
                size = int(self.rfile.readline().split(b";")[0], 16)
                if not size:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    return data, "chunked"
                data += self.rfile.read(size)
                self.rfile.readline()
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n), "length"

    def do_POST(self):
        data, how = self.body()
        if self.path.startswith("/basic") or self.path.startswith("/digest"):
            return self.do_GET()
        self.reply(200, f"{len(data)} {hashlib.md5(data).hexdigest()} {how}".encode())

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        p = self.path
        with LOCK:
            SEEN["ports"].append(self.client_address[1])
        if p.startswith("http://"):                     # absolute form reached the origin: the proxy did not rewrite it
            p = "/" + p.split("/", 3)[3]
        if p == "/hello":
            self.reply(200, b"hello, Amiga\n", [("Set-Cookie", "a=1"), ("Set-Cookie", "b=2")])
        elif p == "/whoami":
            self.reply(200, str(self.client_address[1]).encode())
        elif p == "/headers":
            self.reply(200, "".join(f"{k}: {v}\n" for k, v in self.headers.items()).encode())
        elif p == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(TEXT), 7777):
                part = TEXT[i:i + 7777]
                self.wfile.write(b"%x;ext=1\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\nX-Trailer: yes\r\n\r\n")
        elif p == "/big":
            self.reply(200, BIG)
        elif p == "/gzip":
            self.reply(200, gzip.compress(TEXT), [("Content-Encoding", "gzip")])
        elif p in CODED:
            self.reply(200, CODED[p][0], [("Content-Encoding", CODED[p][1])])
        elif p == "/redirect":
            self.reply(302, b"", [("Location", "/hello")])
        elif p == "/slow":
            time.sleep(2.5)
            self.reply(200, b"slow\n")
        elif p == "/closing":
            self.reply(200, b"bye", [("Connection", "close")])
            self.close_connection = True
        elif p == "/eof":                               # close-delimited
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(TEXT)
            self.close_connection = True
        elif p.startswith("/basic"):
            if self.headers.get("Authorization") == "Basic " + base64.b64encode(b"us er:pa ss").decode():
                self.reply(200, b"basic ok\n")
            else:
                self.reply(401, b"who are you?\n" * 100, [("WWW-Authenticate", 'Basic realm="test"')])
        elif p.startswith("/digest"):
            alg = "SHA-256" if "sha256" in p else "MD5"
            if digest_ok(self.headers.get("Authorization"), self.command, alg):
                self.reply(200, f"digest {alg} ok\n".encode())
            else:
                self.reply(401, b"no\n", [("WWW-Authenticate", f'Digest realm="test@wfnet", qop="auth", algorithm={alg}, '
                                                               f'nonce="{DIGEST_NONCE}", opaque="op"'),
                                          ("WWW-Authenticate", 'Basic realm="test@wfnet"')])
        else:
            self.reply(404, b"no such page\n")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(handler, context=None):
    s = Server(("127.0.0.1", 0), handler)
    if context:
        s.socket = context.wrap_socket(s.socket, server_side=True)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s.server_address[1]


def tls_context(alpn):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(f"{TMP}/srv.pem", f"{TMP}/srv.key")
    ctx.set_alpn_protocols(alpn)
    return ctx


HTTP = serve(Handler)
HTTPS = serve(Handler, tls_context(["http/1.1"]))
base, sbase = f"http://127.0.0.1:{HTTP}", f"https://localhost:{HTTPS}"

# ---- a small HTTP/2 server, by hand: frames, SETTINGS, flow control both ways, HPACK both ways --------

HLEN = [13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28,
        28, 28, 28, 6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12,
        10, 13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6, 15, 5, 6,
        5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5, 6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28, 20, 22, 20, 20, 22, 22,
        22, 23, 22, 23, 23, 23, 23, 23, 24, 23, 24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24, 22, 21, 20,
        22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23, 21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
        26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25, 19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28,
        27, 27, 27, 20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23, 26, 27, 26, 26, 27, 27, 27, 27, 27, 28,
        27, 27, 27, 27, 27, 26, 30]
ORDER = sorted(range(257), key=lambda s: (HLEN[s], s))
CODE = {}
_c, _prev = 0, HLEN[ORDER[0]]
for _i, _s in enumerate(ORDER):
    if _i:
        _c = (_c + 1) << (HLEN[_s] - _prev)
    _prev = HLEN[_s]
    CODE[_s] = _c
DECODE_MAP = {(HLEN[s], CODE[s]): s for s in range(257)}
STATIC = [(":authority", ""), (":method", "GET"), (":method", "POST"), (":path", "/"), (":path", "/index.html"),
          (":scheme", "http"), (":scheme", "https"), (":status", "200"), (":status", "204"), (":status", "206"),
          (":status", "304"), (":status", "400"), (":status", "404"), (":status", "500")] + \
         [(n, "") for n in "accept-charset".split()] + [("accept-encoding", "gzip, deflate")] + \
         [(n, "") for n in ("accept-language accept-ranges accept access-control-allow-origin age allow authorization "
                            "cache-control content-disposition content-encoding content-language content-length "
                            "content-location content-range content-type cookie date etag expect expires from host "
                            "if-match if-modified-since if-none-match if-range if-unmodified-since last-modified link "
                            "location max-forwards proxy-authenticate proxy-authorization range referer refresh "
                            "retry-after server set-cookie strict-transport-security transfer-encoding user-agent vary "
                            "via www-authenticate").split()]
assert len(STATIC) == 61


def py_huff_decode(b):
    out, code, n = bytearray(), 0, 0
    for byte in b:
        for i in range(7, -1, -1):
            code, n = code << 1 | (byte >> i) & 1, n + 1
            s = DECODE_MAP.get((n, code))
            if s is not None:
                assert s != 256
                out.append(s)
                code, n = 0, 0
    assert n < 8 and code == (1 << n) - 1
    return bytes(out)


def py_huff_encode(b):
    acc, bits = 0, 0
    for ch in b:
        acc, bits = acc << HLEN[ch] | CODE[ch], bits + HLEN[ch]
    pad = -bits % 8
    acc, bits = acc << pad | (1 << pad) - 1, bits + pad
    return acc.to_bytes(bits // 8, "big")


class PyHpack:
    def __init__(self):
        self.dyn = []

    def get(self, i):
        return STATIC[i - 1] if i <= 61 else self.dyn[i - 62]

    def add(self, n, v):
        self.dyn.insert(0, (n, v))
        while sum(len(a) + len(b) + 32 for a, b in self.dyn) > 4096:
            self.dyn.pop()

    @staticmethod
    def int_(b, at, bits):
        v = b[at] & ((1 << bits) - 1)
        at += 1
        if v < (1 << bits) - 1:
            return v, at
        m = 0
        while True:
            v += (b[at] & 127) << m
            m += 7
            at += 1
            if not b[at - 1] & 128:
                return v, at

    def str_(self, b, at):
        h = b[at] & 128
        n, at = self.int_(b, at, 7)
        s = b[at:at + n]
        return (py_huff_decode(s) if h else s).decode(), at + n

    def decode(self, b):
        at, out = 0, []
        while at < len(b):
            x = b[at]
            if x & 128:
                i, at = self.int_(b, at, 7)
                out.append(self.get(i))
            elif x & 0xe0 == 0x20:
                _, at = self.int_(b, at, 5)
            else:
                i, at = self.int_(b, at, 6 if x & 0x40 else 4)
                n = self.get(i)[0] if i else None
                if n is None:
                    n, at = self.str_(b, at)
                v, at = self.str_(b, at)
                if x & 0x40:
                    self.add(n, v)
                out.append((n, v))
        return out

    @staticmethod
    def enc_int(first, bits, v):
        lim = (1 << bits) - 1
        if v < lim:
            return bytes([first | v])
        out, v = bytearray([first | lim]), v - lim
        while v >= 128:
            out.append(v & 127 | 128)
            v >>= 7
        out.append(v)
        return bytes(out)

    def encode(self, fields):
        """Indexed when the table has it; else with incremental indexing, Huffman: so the client's
        dynamic table and Huffman decoder are both used."""
        out = b""
        for n, v in fields:
            table = STATIC + self.dyn
            if (n, v) in table:
                out += self.enc_int(0x80, 7, table.index((n, v)) + 1)
                continue
            names = [a for a, _ in table]
            out += self.enc_int(0x40, 6, names.index(n) + 1 if n in names else 0)
            if n not in names:
                hn = py_huff_encode(n.encode())
                out += self.enc_int(0x80, 7, len(hn)) + hn
            hv = py_huff_encode(v.encode())
            out += self.enc_int(0x80, 7, len(hv)) + hv
            self.add(n, v)
        return out


class H2Conn:
    def __init__(self, sock, mode):
        self.s, self.mode = sock, mode
        self.lock = threading.Lock()
        self.cv = threading.Condition(self.lock)
        self.dec, self.enc = PyHpack(), PyHpack()
        self.conn_win, self.init_win, self.wins = 65535, 65535, {}
        self.streams, self.served = {}, 0
        self.closed = False
        with LOCK:
            SEEN["h2serial"] = SEEN.get("h2serial", 0) + 1
            self.serial = SEEN["h2serial"]

    def send(self, typ, flags, sid, payload=b""):
        with self.lock:
            self.s.sendall(struct.pack(">I", len(payload))[1:] + bytes([typ, flags]) + struct.pack(">I", sid) + payload)

    def recv_exact(self, n):
        b = b""
        while len(b) < n:
            got = self.s.recv(n - len(b))
            if not got:
                raise EOFError
            b += got
        return b

    def respond(self, sid, req, body):
        path, method = req.get(":path"), req.get(":method")
        with LOCK:
            SEEN["h2streams"].append((self.serial, sid, path))
        if path.startswith("/slow"):
            time.sleep(0.4)
        if path == "/reset":
            self.send(3, 0, sid, struct.pack(">I", 2))
            return
        if path == "/headers":
            data = "".join(f"{k}: {v}\n" for k, v in req.items()).encode()
        elif path == "/big":
            data = BIG
        elif path == "/gzbig":
            data = GZBIG
        elif path == "/echo":
            data = f"{len(body)} {hashlib.md5(body).hexdigest()}".encode()
        elif path.startswith("/slow") or path.startswith("/s"):
            data = f"stream {sid} {path}".encode()
        else:
            data = b"hello over h2\n"
        status = "404" if path == "/missing" else "200"
        cl = len(data) + (5 if path == "/short" else -5 if path == "/long" else 0)
        fields = [(":status", status), ("server", "pyh2"), ("content-type", "text/plain"),
                  ("content-length", str(cl)), ("x-stream", str(sid)), ("set-cookie", "s=1"), ("set-cookie", "t=2")]
        if path == "/gzbig":
            fields.append(("content-encoding", "gzip"))
        with self.lock:
            block = self.enc.encode(fields)          # encoded in the order sent, so the tables agree
            self.s.sendall(struct.pack(">I", len(block))[1:] + bytes([1, 4 | (1 if method == "HEAD" else 0)])
                           + struct.pack(">I", sid) + block)
        if method == "HEAD":
            return
        at = 0
        while at < len(data) or not data:
            with self.cv:
                while not self.closed and min(self.conn_win, self.wins.get(sid, 0)) <= 0:
                    self.cv.wait(5)
                if self.closed or sid not in self.wins:
                    return
                n = min(16384, self.conn_win, self.wins[sid], len(data) - at)
                self.conn_win -= n
                self.wins[sid] -= n
                last = at + n >= len(data)
                self.s.sendall(struct.pack(">I", n)[1:] + bytes([0, 1 if last else 0]) + struct.pack(">I", sid)
                               + data[at:at + n])
            at += n
            if last:
                break

    def run(self):
        try:
            assert self.recv_exact(24) == b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
            self.send(4, 0, 0, struct.pack(">HI", 3, 100) + struct.pack(">HI", 4, 65535))
            block, bsid = b"", 0
            while True:
                h = self.recv_exact(9)
                n, typ, flags, sid = int.from_bytes(h[:3], "big"), h[3], h[4], int.from_bytes(h[5:], "big") & 0x7fffffff
                p = self.recv_exact(n)
                if typ == 4 and not flags & 1:
                    for i in range(0, n, 6):
                        k, v = struct.unpack(">HI", p[i:i + 6])
                        if k == 4:
                            with self.cv:
                                for s in self.wins:
                                    self.wins[s] += v - self.init_win
                                self.init_win = v
                                self.cv.notify_all()
                        assert not (k == 2 and v != 0)
                    self.send(4, 1, 0)
                elif typ == 8:
                    inc = int.from_bytes(p, "big") & 0x7fffffff
                    with self.cv:
                        if sid == 0:
                            self.conn_win += inc
                        elif sid in self.wins:
                            self.wins[sid] += inc
                        self.cv.notify_all()
                elif typ in (1, 9):
                    if typ == 1:
                        pad = p[0] if flags & 8 else 0
                        p = p[(1 if flags & 8 else 0) + (5 if flags & 0x20 else 0):len(p) - pad]
                        bsid, end_stream = sid, flags & 1
                    block += p
                    if flags & 4:
                        req = {}
                        for k, v in self.dec.decode(block):
                            req[k] = req[k] + ", " + v if k in req and not k.startswith(":") else v
                        block = b""
                        with self.cv:
                            self.wins[bsid] = self.init_win
                        self.streams[bsid] = [req, b""]
                        self.served += 1
                        if self.mode == "goaway" and self.served == 3:
                            # Take the first three streams, refuse the rest by GOAWAY.
                            self.send(7, 0, 0, struct.pack(">II", bsid, 0))
                        if self.mode == "goaway" and self.served > 3:
                            continue
                        if end_stream:
                            threading.Thread(target=self.respond, args=(bsid, req, b""), daemon=True).start()
                elif typ == 0:
                    if sid in self.streams:
                        self.streams[sid][1] += p
                    if n:
                        self.send(8, 0, 0, struct.pack(">I", n))
                        self.send(8, 0, sid, struct.pack(">I", n))
                    if flags & 1 and sid in self.streams:
                        req, body = self.streams[sid]
                        threading.Thread(target=self.respond, args=(sid, req, body), daemon=True).start()
                elif typ == 6 and not flags & 1:
                    self.send(6, 1, 0, p)
                elif typ == 3:
                    with self.cv:
                        self.wins.pop(sid, None)
                        self.cv.notify_all()
                elif typ == 7:
                    break
        except (EOFError, OSError, ssl.SSLError):
            pass
        finally:
            with self.cv:
                self.closed = True
                self.cv.notify_all()
            try:
                self.s.close()
            except OSError:
                pass


def h2_server(mode="normal"):
    ctx = tls_context(["h2", "http/1.1"])
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(64)

    def accept():
        while True:
            c, _ = ls.accept()
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=handshake, args=(c,), daemon=True).start()

    def handshake(c):
        try:
            t = ctx.wrap_socket(c, server_side=True)
        except (ssl.SSLError, OSError):
            return
        if t.selected_alpn_protocol() == "h2":
            with LOCK:
                SEEN["h2conns"] += 1
            H2Conn(t, mode).run()
        else:                                           # HTTP/1.1 for a client that did not offer h2
            data = b""
            while b"\r\n\r\n" not in data:
                got = t.recv(4096)
                if not got:
                    return
                data += got
            t.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nhello, h1.1\n")
            t.close()

    threading.Thread(target=accept, daemon=True).start()
    return ls.getsockname()[1]


H2 = h2_server()
H2GOAWAY = h2_server("goaway")

# ---- a WebSocket echo server ---------------------------------------------------------------------------


def ws_server():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(8)

    def frame(op, payload, fin=True):
        n = len(payload)
        head = bytes([(0x80 if fin else 0) | op])
        head += bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n)
        return head + payload

    def read_frame(c):
        def exact(n):
            b = b""
            while len(b) < n:
                g = c.recv(n - len(b))
                if not g:
                    raise EOFError
                b += g
            return b
        h = exact(2)
        n = h[1] & 127
        assert h[1] & 0x80, "client frames must be masked"
        if n == 126:
            n = struct.unpack(">H", exact(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", exact(8))[0]
        mask = exact(4)
        data = bytes(b ^ mask[i & 3] for i, b in enumerate(exact(n)))
        return h[0] & 0x80, h[0] & 15, data

    def conn(c):
        try:
            req = b""
            while b"\r\n\r\n" not in req:
                req += c.recv(4096)
            hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in req.decode().split("\r\n")[1:] if ":" in l}
            if hdrs.get("upgrade", "").lower() != "websocket" or hdrs.get("sec-websocket-version") != "13":
                c.sendall(b"HTTP/1.1 400 Bad\r\nContent-Length: 0\r\n\r\n")
                return
            key = hdrs["sec-websocket-key"]
            acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            if req.startswith(b"GET /badaccept"):
                acc = "x" * 28
            c.sendall(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {acc}\r\n\r\n".encode() + frame(9, b"are you there?") + frame(1, b"welcome"))
            msg, mop, closing = b"", 0, False
            while True:
                fin, op, data = read_frame(c)
                if op == 10:
                    with LOCK:
                        SEEN["ws_pong"].append(data)
                elif op == 9:
                    c.sendall(frame(10, data))
                elif op == 8:
                    if not closing:                 # the answer to ours ends it; theirs is answered
                        c.sendall(frame(8, data[:2] or b"\x03\xe8"))
                    break
                else:
                    if op:
                        mop = op
                    msg += data
                    if fin:
                        if msg == b"big please":
                            big = bytes(range(256)) * 400
                            c.sendall(frame(2, big[:50000], False) + frame(0, big[50000:]))
                        elif msg == b"close please":
                            c.sendall(frame(8, struct.pack(">H", 4000) + b"done"))
                            closing = True
                        else:
                            c.sendall(frame(mop, msg))
                        msg = b""
        except (EOFError, OSError, AssertionError):
            pass
        finally:
            c.close()

    def accept():
        while True:
            c, _ = ls.accept()
            threading.Thread(target=conn, args=(c,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return ls.getsockname()[1]


WS = ws_server()

# ---- an HTTP proxy: CONNECT tunnels, and absolute-form requests passed on -------------------------------


def proxy_server(need_auth=None):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(16)

    def pipe(a, b):
        try:
            while True:
                d = a.recv(65536)
                if not d:
                    break
                b.sendall(d)
        except OSError:
            pass
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def conn(c):
        try:
            while True:
                req = b""
                while b"\r\n\r\n" not in req:
                    g = c.recv(4096)
                    if not g:
                        return
                    req += g
                head, rest = req.split(b"\r\n\r\n", 1)
                lines_ = head.decode().split("\r\n")
                method, target, _ = lines_[0].split(" ")
                hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines_[1:]}
                with LOCK:
                    SEEN["proxy"].append((method, target, hdrs.get("proxy-authorization")))
                if need_auth == "digest":
                    pa = hdrs.get("proxy-authorization")
                    if not (digest_ok(pa, method, "MD5") and f'uri="{target}"' in pa):
                        c.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Digest '
                                  b'realm="test@wfnet", qop="auth", nonce="' + DIGEST_NONCE.encode() + b'", opaque="op"\r\n'
                                  b"Content-Length: 6\r\n\r\nlogin\n")
                        continue
                elif need_auth and hdrs.get("proxy-authorization") != need_auth:
                    c.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="proxy"\r\n'
                              b"Content-Length: 6\r\n\r\nlogin\n")
                    continue
                if method == "CONNECT":
                    host, port = target.rsplit(":", 1)
                    up = socket.create_connection((host, int(port)))
                    c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                    threading.Thread(target=pipe, args=(up, c), daemon=True).start()
                    pipe(c, up)
                    return
                hp = target.split("/")[2]
                host, port = hp.rsplit(":", 1) if ":" in hp else (hp, "80")
                path = "/" + target.split("/", 3)[3] if target.count("/") >= 3 else "/"
                up = socket.create_connection((host, int(port)))
                out = [f"{method} {path} HTTP/1.1"] + [l for l in lines_[1:] if not l.lower().startswith(("proxy-", "connection"))]
                up.sendall(("\r\n".join(out) + "\r\nConnection: close\r\nVia: 1.1 testproxy\r\n\r\n").encode() + rest)
                pipe(up, c)
                return
        except OSError:
            pass
        finally:
            c.close()

    def accept():
        while True:
            c, _ = ls.accept()
            threading.Thread(target=conn, args=(c,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return ls.getsockname()[1]


PROXY = proxy_server()
PROXY_AUTH = proxy_server("Basic " + base64.b64encode(b"pu:pp").decode())
PROXY_DIGEST = proxy_server("digest")


# ---- an HTTP/1.1 server that answers the first request on a connection and closes on the second, unanswered --


def flaky_server():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(8)

    def conn(c):
        try:
            data = b""
            for n in range(2):
                while b"\r\n\r\n" not in data:
                    g = c.recv(4096)
                    if not g:
                        return
                    data += g
                head, data = data.split(b"\r\n\r\n", 1)
                length = 0
                for l in head.decode().split("\r\n")[1:]:
                    if l.lower().startswith("content-length:"):
                        length = int(l.split(":", 1)[1])
                while len(data) < length:
                    data += c.recv(4096)
                data = data[length:]
                with LOCK:
                    SEEN["flaky"].append(head.split(b" ")[0].decode())
                if n == 0:
                    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n")
        except OSError:
            pass
        finally:
            c.close()

    def accept():
        while True:
            c, _ = ls.accept()
            threading.Thread(target=conn, args=(c,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return ls.getsockname()[1]


SEEN["flaky"] = []
FLAKY = flaky_server()

# ---- HTTP/1.1 ------------------------------------------------------------------------------------------

F = Fetcher()
hd, body, err = F.fetch(f"{base}/hello", headers="Accept: text/plain\n")
assert hd["status"] == 200 and body == b"hello, Amiga\n" and err == 0 and hd["version"] == 11, hd
assert hd["headers"]["set-cookie"] == ["a=1", "b=2"] and hd["flags"] & H_LENGTH and hd["length"] == 13, hd
assert "transport=native" in hd["info"], hd["info"]
hd, body, _ = F.fetch(f"{base}/headers", headers="X-Mine: 1\nHost: wrong\nConnection: close\n")
sent = body.decode().lower()
assert "x-mine: 1" in sent and f"host: 127.0.0.1:{HTTP}" in sent and "connection: keep-alive" in sent and "openweb-fetch/2" in sent, sent
hd, body, _ = F.fetch(f"{base}/redirect")
assert hd["status"] == 302 and hd["headers"]["location"] == ["/hello"], hd
hd, body, _ = F.fetch(f"{base}/missing")
assert hd["status"] == 404 and body == b"no such page\n"
hd, body, _ = F.fetch(f"{base}/hello", method="HEAD")
assert hd["status"] == 200 and hd["flags"] & H_NOBODY and body == b"", hd
print("HTTP/1.1: status, headers (Set-Cookie kept, ours written), 302 left to the caller, 404, HEAD")

# Kept connections: one connection for many fetches, one after another.
SEEN["ports"].clear()
ports = set()
for i in range(20):
    hd, body, _ = F.fetch(f"{base}/whoami")
    ports.add(body)
assert len(ports) == 1, ports
hd, _, _ = F.fetch(f"{base}/whoami")
assert "reused=yes" in hd["info"], hd["info"]
hd, body, _ = F.fetch(f"{base}/closing")
hd, body2, _ = F.fetch(f"{base}/whoami")
assert body2 not in ports, "a connection the server closed was used again"
assert F.stats()[1] == 1, F.stats()
print("kept connections: 20 fetches on one connection (reused=yes); Connection: close honoured")

# A kept connection that closes with no answer: GET and PUT go again; POST is not sent twice.
for method, again in (("GET", True), ("PUT", True), ("POST", False)):
    K = Fetcher()
    SEEN["flaky"].clear()
    hd, body, _ = K.fetch(f"http://127.0.0.1:{FLAKY}/first")
    assert hd["status"] == 200 and body == b"ok\n", hd
    hd, body, _ = K.fetch(f"http://127.0.0.1:{FLAKY}/second", method, body=b"x=1" if method != "GET" else None)
    if again:
        assert hd["status"] == 200 and body == b"ok\n" and SEEN["flaky"] == ["GET", method, method], (hd, SEEN["flaky"])
    else:
        assert hd["error"] == E_CLOSED and SEEN["flaky"] == ["GET", "POST"], (hd, SEEN["flaky"])
    K.free()
print("a kept connection closed with no answer: GET and PUT sent again, POST not")

# Chunked, close-delimited, large bodies, several at once.
hd, body, err = F.fetch(f"{base}/chunked")
assert body == TEXT and err == 0 and not hd["flags"] & H_LENGTH and "transfer-encoding" not in hd["headers"], hd
hd, body, err = F.fetch(f"{base}/eof")
assert body == TEXT and err == 0
hd, body, err = F.fetch(f"{base}/big")
assert hashlib.md5(body).digest() == hashlib.md5(BIG).digest() and err == 0
hs = [F.open(f"GET {base}/big\n") for _ in range(6)] + [F.open(f"GET {base}/chunked\n") for _ in range(4)]
got = {h: b"" for h in hs}
done = set()
t = time.time()
while len(done) < len(hs) and time.time() - t < 60:
    for h in hs:
        if h in done:
            continue
        st, n, end, out = F.call(READ, h, (50, 0, 0, 0), room=65536)
        got[h] += out
        if end:
            assert end >> 8 == 0
            done.add(h)
assert all(got[h] == BIG for h in hs[:6]) and all(got[h] == TEXT for h in hs[6:])
for h in hs:
    F.close(h)
print("chunked (with extensions and trailers), close-delimited, 3 MB, and 10 fetches at once read side by side")

# Uploads: with a length, chunked through SEND, and large.
data = random.randbytes(300000)
hd, body, _ = F.fetch(f"{base}/upload", "POST", body=data)
assert body.decode() == f"{len(data)} {hashlib.md5(data).hexdigest()} length", body
h = F.open(f"POST {base}/upload\n", BODY, UNKNOWN, 0, data[:1000])
at = 1000
while at < len(data):
    part = data[at:at + 50000]
    st, taken, _, _ = F.call(SEND, h, (1 if at + len(part) >= len(data) else 0, 1000, 0, 0), (part,))
    assert st == 0
    at += taken
hd = F.head(h)
body, _ = F.read_all(h)
F.close(h)
assert body.decode() == f"{len(data)} {hashlib.md5(data).hexdigest()} chunked", body
print("uploads: 300 KB with Content-Length, and the same chunked through SEND")

# Errors: DNS, refused, timeout, cancel.
hd, _, _ = F.fetch("http://no-such-host.invalid/")
assert hd["error"] == E_DNS, hd
dead = socket.socket()
dead.bind(("127.0.0.1", 0))
dport = dead.getsockname()[1]
dead.close()
hd, _, _ = F.fetch(f"http://127.0.0.1:{dport}/")
assert hd["error"] == E_CONNECT, hd
t = time.time()
hd, _, _ = F.fetch(f"{base}/slow", timeout=1)
assert hd["error"] == E_TIMEOUT and 0.9 < time.time() - t < 2.4, (hd, time.time() - t)
h = F.open(f"GET {base}/slow\n")
F.call(HEAD, h, (200, 0, 0, 0), room=4096)
F.call(CANCEL, h)
hd = F.head(h)
assert hd["error"] == E_CANCELLED and "error=cancelled" in hd["info"], hd
F.close(h)
assert F.call(HEAD, 12345, (0, 0, 0, 0), room=100)[0] == -2
assert F.call(OPEN, 0, (0, 0, 0, 0), (b"GET ftp://x/\n",))[0] == -2
print("errors: DNS, refused, a timeout of 1 s, CANCEL; bad handles and URLs refused (-2)")

# A name with two addresses, the first refusing after the connect started: the second is tried.
os.environ["WFNP_HOSTS"] = "two.test 127.0.0.2 127.0.0.1"
hd, body, _ = F.fetch(f"http://two.test:{HTTP}/hello")
assert hd["status"] == 200 and body == b"hello, Amiga\n", hd
os.environ["WFNP_HOSTS"] = "none.test 127.0.0.2 127.0.0.3"
hd, body, _ = F.fetch(f"http://none.test:{HTTP}/hello")
assert hd["error"] == E_CONNECT, hd
os.environ.pop("WFNP_HOSTS")
print("connect: the next address when the first fails on the way; refused when none takes it")

# Content-Encoding: kept as sent, or through the decoder hook (zlib standing in for the datatypes).
hd, body, _ = F.fetch(f"{base}/gzip", flags=DECODE)
assert gzip.decompress(body) == TEXT and hd["headers"]["content-encoding"] == ["gzip"] and "decode=datatypes" in hd["info"], hd
assert not hd["flags"] & H_DECODED
hd, body, _ = F.fetch(f"{base}/gzip")
assert gzip.decompress(body) == TEXT and "decode=" not in hd["info"]
D = Fetcher(decoders=True)
hd, body, _ = D.fetch(f"{base}/gzip", flags=DECODE)
assert body == TEXT and hd["flags"] & H_DECODED and "content-encoding" not in hd["headers"], hd
assert "decode=zlib" in hd["info"], hd["info"]
hd, body, _ = F.fetch(f"{base}/hello", flags=DECODE)
assert "decode=" not in hd["info"], hd["info"]                 # nothing to undo: nothing said
# Accept-Encoding offers only what the decoders undo: identity with none, the caller's own dropped.
assert "accept-encoding: identity\n" in F.fetch(f"{base}/headers", headers="Accept-Encoding: zstd\n", flags=DECODE)[1].decode().lower()
assert "accept-encoding: gzip, deflate\n" in D.fetch(f"{base}/headers", flags=DECODE)[1].decode().lower()
assert "accept-encoding: zstd\n" in F.fetch(f"{base}/headers", headers="Accept-Encoding: zstd\n")[1].decode().lower()
print("Content-Encoding: passed on as sent (decode=datatypes when WF_DECODE has no decoder); through the hook when there "
      "is one (decode=zlib); Accept-Encoding: identity with no decoder, what the hook undoes with one")

# One task, many fetches, never blocking: every call with a wait of 0, and wfn_pump to sleep on all of them.
LIB.wfnp_pump.argtypes = [ctypes.c_void_p, UL]
hs = [F.open(f"GET {base}/slow\n") for _ in range(2)] + [F.open(f"GET {base}/chunked\n") for _ in range(6)]
got, done, calls = {h: b"" for h in hs}, set(), 0
t = time.time()
while len(done) < len(hs) and time.time() - t < 20:
    LIB.wfnp_pump(F.w, 200)
    for h in hs:
        if h in done:
            continue
        c0 = time.time()
        st, n, end, out = F.call(READ, h, (0, 0, 0, 0), room=65536)
        assert time.time() - c0 < 0.05, "a call with a wait of 0 blocked"
        got[h] += out
        calls += 1
        if end:
            done.add(h)
assert [got[h] for h in hs] == [b"slow\n"] * 2 + [TEXT] * 6 and time.time() - t < 4, time.time() - t
for h in hs:
    F.close(h)
print(f"one task, 8 fetches, every call with a wait of 0 and wfn_pump between: {calls} calls, none blocked")

# ---- HTTPS, HTTP/1.1 -----------------------------------------------------------------------------------

hd, body, _ = F.fetch(f"{sbase}/hello")
assert hd["status"] == 200 and hd["flags"] & H_TLS and hd["flags"] & H_VERIFIED and "tls=TLSv1.3" in hd["info"], hd
assert "alpn=h2" not in hd["info"]
hd2, _, _ = F.fetch(f"{sbase}/hello")
assert "reused=yes" in hd2["info"] and "verified=yes" in hd2["info"]
hd, _, _ = F.fetch(f"https://127.0.0.1:{HTTPS}/hello")
assert hd["status"] == 200, hd
print("HTTPS over HTTP/1.1: certificate checked by name and by IP, the connection kept")

# ---- HTTP/2 --------------------------------------------------------------------------------------------

h2base = f"https://localhost:{H2}"
SEEN["h2conns"] = 0
hd, body, err = F.fetch(f"{h2base}/hello")
assert hd["status"] == 200 and hd["version"] == 20 and body == b"hello over h2\n" and "alpn=h2" in hd["info"], hd
assert hd["text"].startswith("HTTP/2 200\n") and hd["headers"]["server"] == ["pyh2"], hd["text"]
assert hd["headers"]["set-cookie"] == ["s=1", "t=2"] and hd["flags"] & H_LENGTH and hd["length"] == 14, hd
hd, body, _ = F.fetch(f"{h2base}/headers", headers="X-Mine: yes\nAccept: */*\nConnection: close\n")
req = body.decode()
assert ":method: GET" in req and ":scheme: https" in req and f":authority: localhost:{H2}" in req and ":path: /headers" in req, req
assert "x-mine: yes" in req and "connection" not in req and "user-agent: OpenWeb-Fetch/2" in req, req
hd, body, _ = F.fetch(f"{h2base}/missing")
assert hd["status"] == 404
hd, body, _ = F.fetch(f"{h2base}/hello", method="HEAD")
assert hd["status"] == 200 and body == b"" and hd["flags"] & H_NOBODY
assert SEEN["h2conns"] == 1, SEEN["h2conns"]
print("HTTP/2 by ALPN: status line HTTP/2, headers through HPACK both ways (the server's dynamic table and "
      "Huffman), pseudo-headers sent, HEAD, 404, one connection")

# Many streams at once on one connection: 12 slow answers take about as long as one.
SEEN["h2conns"] = 0
SEEN["h2streams"].clear()
t = time.time()
hs = [F.open(f"GET https://localhost:{H2}/slow{i}\n") for i in range(12)]
bodies = {}
while len(bodies) < len(hs) and time.time() - t < 20:
    for i, h in enumerate(hs):
        if h in bodies:
            continue
        st, status, e, out = F.call(HEAD, h, (20, 0, 0, 0), room=8192)
        if status:
            bodies[h] = F.read_all(h)[0]
took = time.time() - t
conns = {c for c, _, _ in SEEN["h2streams"]}
assert all(bodies[h].startswith(b"stream ") and bodies[h].endswith(f"/slow{i}".encode()) for i, h in enumerate(hs)), bodies
assert len(conns) == 1 and SEEN["h2conns"] <= 1 and took < 2.0, (conns, SEEN["h2conns"], took)
sids = sorted(s for _, s, _ in SEEN["h2streams"])
assert len(set(sids)) == 12 and all(s % 2 for s in sids)
for h in hs:
    F.close(h)
print(f"HTTP/2 streams side by side: 12 answers of 0.4 s each in {took:.2f} s on one connection")

# Flow control: 3 MB on a stream with a 64 KB window, read slowly; several large ones at once.
hd, body, err = F.fetch(f"{h2base}/big")
assert body == BIG and err == 0
hs = [F.open(f"GET {h2base}/big\n") for _ in range(4)]
got = {h: b"" for h in hs}
done = set()
t = time.time()
while len(done) < len(hs) and time.time() - t < 60:
    for h in hs:
        if h not in done:
            st, n, end, out = F.call(READ, h, (50, 0, 0, 0), room=20000)
            got[h] += out
            if end:
                done.add(h)
assert all(got[h] == BIG for h in hs)
for h in hs:
    F.close(h)
print("HTTP/2 flow control: 3 MB through a 64 KB stream window; four at once, read 20 KB at a time")

# Uploads over HTTP/2, with a length and streamed.
data = random.randbytes(200000)
hd, body, _ = F.fetch(f"{h2base}/echo", "POST", body=data)
assert body.decode() == f"{len(data)} {hashlib.md5(data).hexdigest()}", body
h = F.open(f"POST {h2base}/echo\n", BODY, UNKNOWN, 0, b"")
for i in range(0, len(data), 40000):
    st, taken, _, _ = F.call(SEND, h, (1 if i + 40000 >= len(data) else 0, 1000, 0, 0), (data[i:i + 40000],))
    assert st == 0 and taken == len(data[i:i + 40000])
F.head(h)
assert F.read_all(h)[0].decode() == f"{len(data)} {hashlib.md5(data).hexdigest()}"
F.close(h)
print("HTTP/2 uploads: 200 KB with content-length, and streamed with SEND, both through the server's windows")

# RST_STREAM ends one stream; the others go on.
hd, body, err = F.fetch(f"{h2base}/reset")
assert hd["error"] == E_CLOSED, hd
hd, body, _ = F.fetch(f"{h2base}/hello")
assert body == b"hello over h2\n"
# content-length against the DATA: short at END_STREAM, or more DATA than it says.
hd, body, err = F.fetch(f"{h2base}/short")
assert hd["status"] == 200 and err == E_CLOSED, (hd, err)
hd, body, err = F.fetch(f"{h2base}/long")
assert err == E_PROTOCOL, (hd, err)
hd, body, _ = F.fetch(f"{h2base}/hello")
assert body == b"hello over h2\n"
# Header names in lower case, however long.
long_name = "X-" + "Long" * 20
hd, body, _ = F.fetch(f"{h2base}/headers", headers=f"{long_name}: 1\n")
assert f"{long_name.lower()}: 1" in body.decode(), body
# Cancel a stream in flight: the connection stays.
h = F.open(f"GET {h2base}/big\n")
F.head(h)
F.call(CANCEL, h)
F.close(h)
hd, body, _ = F.fetch(f"{h2base}/hello")
assert body == b"hello over h2\n"
# GOAWAY after three streams: the rest go again on a new connection.
SEEN["h2streams"].clear()
G = Fetcher()
hs = [G.open(f"GET https://localhost:{H2GOAWAY}/s{i}\n") for i in range(6)]
res = []
for i, h in enumerate(hs):
    hd = G.head(h)
    res.append((hd["status"], G.read_all(h)[0]))
assert all(s == 200 and b.endswith(f"/s{i}".encode()) for i, (s, b) in enumerate(res)), res
assert len({c for c, _, _ in SEEN["h2streams"]}) == 2, SEEN["h2streams"]
G.free()
# HTTP/1.1 only, by flag: the server falls back by ALPN.
hd, body, _ = F.fetch(f"{h2base}/hello", flags=HTTP1)
assert hd["version"] == 11 and body == b"hello, h1.1\n", hd
print("HTTP/2: RST_STREAM fails one stream, content-length checked against the DATA, long header names lower-cased, "
      "CANCEL resets one, GOAWAY sends the untaken streams again on a new "
      "connection; WF_HTTP1 offers http/1.1 alone")

# ---- WebSocket -----------------------------------------------------------------------------------------


def ws_recv(Fx, h, room=65536, wait=1000, limit=10):
    t = time.time()
    while time.time() - t < limit:
        st, n, aux, out = Fx.call(WS_RECV, h, (wait, 0, 0, 0), room=room)
        assert st == 0, st
        if n or aux:
            return out, aux
    raise AssertionError("nothing")


def ws_send(Fx, h, op, data):
    st, n, aux, _ = Fx.call(WS_SEND, h, (op, 1000, 0, 0), (data,))
    assert st == 0 and n == len(data), (st, n, aux)


SEEN["ws_pong"].clear()
h = F.open(f"GET ws://127.0.0.1:{WS}/chat\nSec-WebSocket-Protocol: chat\n", ws=True)
hd = F.head(h)
assert hd["status"] == 101 and hd["headers"]["upgrade"] == ["websocket"], hd
assert ws_recv(F, h) == (b"welcome", WS_TEXT | WS_FIN)       # the server's ping was answered on the way
ws_send(F, h, WS_TEXT, "hello, echo ✓".encode())
assert ws_recv(F, h) == ("hello, echo ✓".encode(), WS_TEXT | WS_FIN)
blob = random.randbytes(70000)
ws_send(F, h, WS_BINARY, blob)
got, aux = b"", 0
while not aux & WS_FIN:                         # larger than the ring: it comes in parts, FIN on the last
    out, aux = ws_recv(F, h, room=100000)
    assert aux & 15 == WS_BINARY, aux
    got += out
assert got == blob
ws_send(F, h, WS_TEXT | WS_MORE, b"in ")
ws_send(F, h, WS_TEXT | WS_MORE, b"three ")
ws_send(F, h, WS_TEXT, b"parts")
assert ws_recv(F, h) == (b"in three parts", WS_TEXT | WS_FIN)
ws_send(F, h, WS_PING, b"ping!")
assert ws_recv(F, h) == (b"ping!", WS_PONG | WS_FIN)
ws_send(F, h, WS_TEXT, b"big please")
parts, aux = b"", 0
while not aux & WS_FIN:
    out, aux = ws_recv(F, h, room=7000)
    assert aux & 15 == WS_BINARY, aux
    parts += out
assert parts == bytes(range(256)) * 400
assert SEEN["ws_pong"] == [b"are you there?"], SEEN["ws_pong"]
ws_send(F, h, WS_TEXT, b"close please")
out, aux = ws_recv(F, h)
assert aux & 15 == WS_CLOSE and aux >> 16 == 4000 and out == b"done", (out, hex(aux))
out, aux = ws_recv(F, h)
assert aux & WS_END and (aux >> 8) & 255 == 0, hex(aux)
F.close(h)
# We close: the server answers, the end is clean.
h = F.open(f"GET ws://127.0.0.1:{WS}/\n", ws=True)
assert F.head(h)["status"] == 101
ws_recv(F, h)
ws_send(F, h, WS_CLOSE, struct.pack(">H", 1000))
out, aux = ws_recv(F, h)
assert aux & 15 == WS_CLOSE and aux >> 16 == 1000, hex(aux)
F.close(h)
# A wrong Sec-WebSocket-Accept is refused.
h = F.open(f"GET ws://127.0.0.1:{WS}/badaccept\n", ws=True)
hd = F.head(h)
assert hd["error"] == E_PROTOCOL and "Sec-WebSocket-Accept" in hd["info"], hd
F.close(h)
print("WebSocket: 101 checked (Sec-WebSocket-Accept), text, 70 KB binary, a message in three parts, ping/pong both "
      "ways, a 100 KB message in pieces, close from either side with its code; a wrong accept refused")

# ---- proxies -------------------------------------------------------------------------------------------

SEEN["proxy"].clear()
hd, body, _ = F.fetch(f"{base}/hello", headers=f":proxy http://127.0.0.1:{PROXY}\n")
assert body == b"hello, Amiga\n" and SEEN["proxy"][-1][:2] == ("GET", f"{base}/hello"), SEEN["proxy"]
assert f"proxy=127.0.0.1:{PROXY}" in hd["info"]
hd, body, _ = F.fetch(f"{sbase}/hello", headers=f":proxy 127.0.0.1:{PROXY}\n")
assert body == b"hello, Amiga\n" and hd["flags"] & H_VERIFIED and SEEN["proxy"][-1][:2] == ("CONNECT", f"localhost:{HTTPS}"), SEEN["proxy"]
hd, body, _ = F.fetch(f"{h2base}/hello", headers=f":proxy http://127.0.0.1:{PROXY}\n")
assert body == b"hello over h2\n" and "alpn=h2" in hd["info"] and SEEN["proxy"][-1][0] == "CONNECT"
h = F.open(f"GET ws://127.0.0.1:{WS}/\n:proxy http://127.0.0.1:{PROXY}\n", ws=True)
assert F.head(h)["status"] == 101 and ws_recv(F, h)[0] == b"welcome" and SEEN["proxy"][-1][0] == "CONNECT"
F.close(h)
print("proxy: http:// in absolute form, https:// (HTTP/1.1 and HTTP/2) and ws:// through CONNECT")

# From the environment, with no_proxy.
os.environ["https_proxy"] = f"http://127.0.0.1:{PROXY}"
os.environ["http_proxy"] = f"http://127.0.0.1:{PROXY}"
E = Fetcher()
n = len(SEEN["proxy"])
E.fetch(f"{sbase}/hello")
E.fetch(f"{base}/hello")
assert len(SEEN["proxy"]) == n + 2
os.environ["no_proxy"] = "example.org, 127.0.0.1,.localhost"
E.fetch(f"{base}/hello")
E.fetch(f"https://localhost:{HTTPS}/hello")
E.fetch(f"{base}/hello", flags=NOPROXY)
assert len(SEEN["proxy"]) == n + 2, SEEN["proxy"][n:]
os.environ["no_proxy"] = "*"
E.fetch(f"{base}/hello")
assert len(SEEN["proxy"]) == n + 2
for k in ("https_proxy", "http_proxy", "no_proxy"):
    os.environ.pop(k)
E.free()
# A proxy that wants a login: 407 is answered when the proxy URL carries one.
hd, body, _ = F.fetch(f"{sbase}/hello", headers=f":proxy http://127.0.0.1:{PROXY_AUTH}\n")
assert hd["status"] == 407 and body == b"login\n", hd
hd, body, _ = F.fetch(f"{sbase}/hello", headers=f":proxy http://pu:pp@127.0.0.1:{PROXY_AUTH}\n")
assert hd["status"] == 200 and body == b"hello, Amiga\n" and "auth=proxy" in hd["info"], hd
hd, body, _ = F.fetch(f"{base}/hello", headers=f":proxy http://pu:pp@127.0.0.1:{PROXY_AUTH}\n")
assert hd["status"] == 200 and body == b"hello, Amiga\n", hd
SEEN["proxy"].clear()
hd, body, _ = F.fetch(f"{base}/hello", headers=f":proxy http://us%20er:pa%20ss@127.0.0.1:{PROXY_DIGEST}\n")
assert hd["status"] == 200 and body == b"hello, Amiga\n" and "auth=proxy" in hd["info"], (hd, SEEN["proxy"])
assert f'uri="{base}/hello"' in SEEN["proxy"][-1][2], SEEN["proxy"]
hd, body, _ = F.fetch(f"{sbase}/hello", headers=f":proxy http://us%20er:pa%20ss@127.0.0.1:{PROXY_DIGEST}\n")
assert hd["status"] == 200 and body == b"hello, Amiga\n", (hd, SEEN["proxy"])
assert f'uri="localhost:{HTTPS}"' in SEEN["proxy"][-1][2], SEEN["proxy"]
print("proxy from http_proxy and https_proxy, no_proxy by name, domain and *, WF_NOPROXY; 407 answered with Basic "
      "for CONNECT and for absolute-form requests, and with Digest over the absolute URI or the authority")

# ---- HTTP auth -----------------------------------------------------------------------------------------

hd, body, _ = F.fetch(f"{base}/basic")
assert hd["status"] == 401
hd, body, _ = F.fetch(f"http://us%20er:pa%20ss@127.0.0.1:{HTTP}/basic")
assert hd["status"] == 200 and body == b"basic ok\n" and "auth=server" in hd["info"], hd
hd, body, _ = F.fetch(f"http://us%20er:wrong@127.0.0.1:{HTTP}/basic")
assert hd["status"] == 401 and body.startswith(b"who are you?"), hd
for path, alg in (("/digest", "MD5"), ("/digest-sha256", "SHA-256")):
    hd, body, _ = F.fetch(f"http://us%20er:pa%20ss@127.0.0.1:{HTTP}{path}")
    assert hd["status"] == 200 and body == f"digest {alg} ok\n".encode(), (path, hd, body)
data = b"form=data&x=1"
hd, body, _ = F.fetch(f"http://us%20er:pa%20ss@127.0.0.1:{HTTP}/digest-sha256", "POST", body=data)
assert hd["status"] == 200, hd
hd, body, _ = F.fetch(f"https://us%20er:pa%20ss@localhost:{HTTPS}/digest")
assert hd["status"] == 200 and body == b"digest MD5 ok\n", hd
print("auth: Basic, and Digest with MD5 and SHA-256 (a POST too, its body sent again), over HTTP and HTTPS; "
      "wrong credentials give the 401 back")

# ---- compressed bodies through the datatypes: src/wfnet_dec.c over stream.datatype's core --------------
# The Amiga's own glue, built with opendatatypes' portable cores (stream/sdtcore.c, gzip/gzcore.c on the
# host's zlib in z.library's place, brotli/brdecode.c) behind host/wfnet_dt.c's NewDTObject() and methods.
#   ODT         the opendatatypes checkout (default: beside this one); without it this part is skipped
#   BROTLI_RFC  RFC 7932's text or its dictionary, for brotli.datatype's (fetched when missing; else no br)
#   CFLAGS      more flags for that build (-fsanitize=address,undefined, with the runtime preloaded)

ODT = os.environ.get("ODT") or os.path.join(HERE, "..", "..", "opendatatypes")
DT = os.path.join(ODT, "Datatypes")
if os.path.isfile(os.path.join(DT, "stream", "sdtcore.c")):
    root = os.path.join(HERE, "..")
    srcs = [os.path.join(root, f) for f in ("host/wfnet_posix.c", "host/wfnet_dt.c", "src/wfnet.c", "src/wfnet_h2.c",
                                            "src/wfnet_hpack.c", "src/wfnet_auth.c", "src/wfnet_dec.c")]
    srcs += [os.path.join(DT, f) for f in ("stream/sdtcore.c", "gzip/gzcore.c", "tests/zlhost.c")]
    rfc = os.environ.get("BROTLI_RFC") or next((p for p in (os.path.join(ODT, "tarballs", "rfc7932.txt"),
                                                            os.path.join(DT, "tarballs", "rfc7932.txt")) if os.path.isfile(p)), None)
    if not rfc and subprocess.run(["curl", "-fsSL", "--retry", "2", "-o", f"{TMP}/rfc7932.txt",
                                   "https://www.rfc-editor.org/rfc/rfc7932.txt"], capture_output=True).returncode == 0:
        rfc = f"{TMP}/rfc7932.txt"
    BR = bool(rfc) and subprocess.run([sys.executable, os.path.join(DT, "brotli", "mkbrdict.py"), rfc, f"{TMP}/brdict.c"],
                                      capture_output=True).returncode == 0
    if BR:
        srcs += [os.path.join(DT, "brotli", "brdecode.c"), f"{TMP}/brdict.c"]
    subprocess.run(["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", "-DWFNP_DATATYPES",
                    "-DZLHOST_SYSTEM"] + (["-DWITH_BROTLI"] if BR else []) + [f"-I{root}/include", f"-I{root}/src"]
                   + [f"-I{DT}/{d}" for d in ("tests/host", "common", "stream", "gzip", "brotli")]
                   + os.environ.get("CFLAGS", "").split() + ["-o", f"{TMP}/libwfnet_dt.so"] + srcs
                   + ["-lssl", "-lcrypto", "-lz"], check=True)
    # The portable parts as the Amiga builds them: C99, warnings as errors, -Os, no writable data.
    for f in ("wfnet", "wfnet_h2", "wfnet_hpack", "wfnet_auth", "wfnet_dec"):
        subprocess.run(["gcc", "-std=c99", "-Os", "-Wall", "-Wextra", "-Werror", f"-I{root}/include", f"-I{root}/src", "-c",
                        f"{root}/src/{f}.c", "-o", f"{TMP}/{f}.o"], check=True)
        sec = subprocess.run(["size", "-A", f"{TMP}/{f}.o"], capture_output=True, text=True, check=True).stdout.split("\n")
        assert not [l for l in sec if l.split()[:1] and l.split()[0].startswith((".data", ".bss")) and int(l.split()[1])], (f, sec)
    DL = ctypes.CDLL(f"{TMP}/libwfnet_dt.so")
    DL.wfnp_new.restype = ctypes.c_void_p
    DL.wfnp_new.argtypes = [UL, ctypes.c_int]
    DL.wfnp_call.restype = ctypes.c_long
    DL.wfnp_call.argtypes = LIB.wfnp_call.argtypes
    DL.wfnp_free.argtypes = [ctypes.c_void_p]
    DL.wfnp_dt_live.restype = ctypes.c_long
    DL.wfn_dec_new.restype = ctypes.c_void_p
    DL.wfn_dec_new.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    DL.wfn_dec_run.restype = ctypes.c_long
    DL.wfn_dec_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, UL, ctypes.POINTER(UL), ctypes.c_char_p, UL,
                               ctypes.c_int]
    DL.wfn_dec_free.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    DL.wfn_dec_name.restype = ctypes.c_char_p
    DL.wfn_dec_name.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    def undo(coding, data, piece, room):
        """The glue driven as wfnet drives it: pieces in, rooms out, then last until nothing more."""
        d = DL.wfn_dec_new(None, coding.encode())
        if not d:
            return None
        out, at, used, buf = [], 0, UL(), ctypes.create_string_buffer(room)
        for _ in range(4 * len(data) + 4 * len(TEXT) + 100):
            last = at >= len(data)
            part = b"" if last else data[at:at + piece]
            n = DL.wfn_dec_run(None, d, part, len(part), ctypes.byref(used), buf, room, int(last))
            if n < 0:
                DL.wfn_dec_free(None, d)
                return "error"
            out.append(buf.raw[:n])
            at += used.value
            if last and not n:
                break
        else:
            raise AssertionError("stuck")
        DL.wfn_dec_free(None, d)
        return b"".join(out)

    def deflate_raw(b):
        c = zlib.compressobj(6, zlib.DEFLATED, -15)
        return c.compress(b) + c.flush()

    VEC = {}                                # opendatatypes' Brotli vectors, made with Google's encoder
    with open(os.path.join(DT, "tests", "brotli-vectors.txt")) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                name, coded, plain = line.split()
                VEC[name] = (bytes.fromhex(coded), b"" if plain == "-" else None if plain == "error" else bytes.fromhex(plain))
    gz = gzip.compress(TEXT)
    cases = [("gzip", gz, TEXT), ("x-gzip", gz, TEXT), ("GZIP", gzip.compress(b"a") + gzip.compress(b"b"), b"ab"),
             ("deflate", zlib.compress(TEXT), TEXT), ("deflate", deflate_raw(TEXT), TEXT),
             ("deflate, gzip", gzip.compress(zlib.compress(TEXT)), TEXT), ("identity, gzip", gz, TEXT),
             ("gzip", gz[:len(gz) // 2], None), ("gzip", gz[:200] + bytes(64) + gz[264:], None), ("gzip", b"", b""),
             ("deflate", b"", b""), ("compress", b"x", "refused"), ("gzip, gzip, gzip, gzip", b"x", "refused")]
    if BR:
        cases += [("br", VEC[v][0], VEC[v][1]) for v in ("vec-html-q11", "vec-repeat-w10", "vec-utf8-q9", "vec-empty-q11")]
        cases += [("br, gzip", gzip.compress(VEC["vec-html-q11"][0]), VEC["vec-html-q11"][1]), ("br", VEC["vec-cut"][0], None)]
    runs = 0
    for coding, data, plain in cases:
        for piece, room in ((65536, 65536), (1, 1), (7, 4096), (4096, 13)):
            if piece == room == 1 and len(data) > 4000:
                continue
            got = undo(coding, data, piece, room)
            if plain == "refused":
                assert got is None, (coding, got)
            elif plain is None:
                assert got == "error", (coding, piece, room, got and got[:40])
            else:
                assert got == plain, (coding, piece, room, len(got) if isinstance(got, bytes) else got)
            runs += 1
    d = DL.wfn_dec_new(None, b"br, gzip" if BR else b"deflate, gzip")
    assert DL.wfn_dec_name(None, d) == (b"gzip.datatype, brotli.datatype" if BR else b"gzip.datatype, gzip.datatype")
    DL.wfn_dec_free(None, d)
    assert DL.wfnp_dt_live() == 0
    print(f"datatypes' glue: {runs} decodes of gzip, x-gzip, gzip members, zlib and raw deflate, two codings in a row"
          f"{', Brotli and br then gzip' if BR else ''}, fed and taken whole and a byte at a time; damaged and cut short "
          "fail; an empty body is no error; an unknown coding or more than three refused" + ("" if BR else
          " (Brotli skipped: no RFC 7932 text, set BROTLI_RFC)"))

    # Through the transport: what it asks for, what it undoes, and what it leaves when a datatype is missing.
    CODED.update({"/c/gzip": (gz, "gzip"), "/c/zlib": (zlib.compress(TEXT), "deflate"),
                  "/c/raw": (deflate_raw(TEXT), "deflate"), "/c/twice": (gzip.compress(zlib.compress(TEXT)), "deflate, gzip"),
                  "/c/bad": (gz[:200] + bytes(64) + gz[264:], "gzip"), "/c/short": (gz[:len(gz) // 2], "gzip"),
                  "/c/empty": (b"", "gzip"), "/c/odd": (b"plain", "compress"), "/c/gzbig": (GZBIG, "gzip")})
    if BR:
        CODED.update({"/c/br": (VEC["vec-html-q11"][0], "br"),
                      "/c/brgz": (gzip.compress(VEC["vec-html-q11"][0]), "br, gzip")})
    G = Fetcher(decoders=True, lib=DL)
    accept = "gzip, deflate, br" if BR else "gzip, deflate"
    assert f"accept-encoding: {accept}\n" in G.fetch(f"{base}/headers", flags=DECODE)[1].decode().lower()
    assert f"accept-encoding: {accept}\n" in G.fetch(f"{h2base}/headers", flags=DECODE)[1].decode().lower()
    for path, plain, name in [("/c/gzip", TEXT, "gzip.datatype"), ("/c/zlib", TEXT, "gzip.datatype"),
                              ("/c/raw", TEXT, "gzip.datatype"), ("/c/twice", TEXT, "gzip.datatype, gzip.datatype"),
                              ("/c/empty", b"", "gzip.datatype")] + \
                             ([("/c/br", VEC["vec-html-q11"][1], "brotli.datatype"),
                               ("/c/brgz", VEC["vec-html-q11"][1], "gzip.datatype, brotli.datatype")] if BR else []):
        for u in (base, sbase):
            hd, body, err = G.fetch(f"{u}{path}", flags=DECODE)
            assert body == plain and err == 0 and hd["flags"] & H_DECODED and not hd["flags"] & H_LENGTH, (path, hd)
            assert "content-encoding" not in hd["headers"] and "content-length" not in hd["headers"], hd
            assert f"decode={name}\n" in hd["info"], (path, hd["info"])
    hd, body, err = G.fetch(f"{base}/c/gzip", "HEAD", flags=DECODE)
    assert hd["status"] == 200 and err == 0 and body == b"", hd
    for path in ("/c/bad", "/c/short"):
        hd, body, err = G.fetch(f"{base}{path}", flags=DECODE)
        assert err == E_DECODE, (path, hd, err)
    hd, body, err = G.fetch(f"{base}/c/odd", flags=DECODE)
    assert body == b"plain" and hd["headers"]["content-encoding"] == ["compress"] and "decode=datatypes" in hd["info"], hd
    hd, body, _ = G.fetch(f"{base}/c/gzip")
    assert body == gz and "decode=" not in hd["info"], hd             # no WF_DECODE: as sent
    S = Fetcher(window=16384, decoders=True, lib=DL)                   # 3 MB through a 16 KB window, HTTP/1.1 and HTTP/2
    for u in (f"{base}/c/gzbig", f"{h2base}/gzbig"):
        hs = [S.open(f"GET {u}\n", DECODE) for _ in range(3)]
        for h in hs:
            assert S.head(h)["flags"] & H_DECODED
        for h in hs:
            body, err = S.read_all(h, room=5000)
            assert err == 0 and hashlib.md5(body).digest() == hashlib.md5(BIG).digest(), (u, len(body), err)
            S.close(h)
    S.free()
    G.free()
    # A datatype that is not installed: not asked for, and its bodies come as sent.
    os.environ["WFNP_DT_MISSING"] = "brotli"
    M = Fetcher(decoders=True, lib=DL)
    assert "accept-encoding: gzip, deflate\n" in M.fetch(f"{base}/headers", flags=DECODE)[1].decode().lower()
    if BR:
        hd, body, _ = M.fetch(f"{base}/c/br", flags=DECODE)
        assert body == VEC["vec-html-q11"][0] and hd["headers"]["content-encoding"] == ["br"], hd
        assert "decode=datatypes" in hd["info"] and not hd["flags"] & H_DECODED, hd
        hd, body, _ = M.fetch(f"{base}/c/brgz", flags=DECODE)
        assert body == CODED["/c/brgz"][0] and hd["headers"]["content-encoding"] == ["br, gzip"], hd
    assert M.fetch(f"{base}/c/gzip", flags=DECODE)[1] == TEXT
    M.free()
    os.environ["WFNP_DT_MISSING"] = "gzip brotli"
    M = Fetcher(decoders=True, lib=DL)
    assert "accept-encoding: identity\n" in M.fetch(f"{base}/headers", flags=DECODE)[1].decode().lower()
    assert "accept-encoding: identity\n" in M.fetch(f"{h2base}/headers", flags=DECODE)[1].decode().lower()
    hd, body, _ = M.fetch(f"{base}/c/gzip", flags=DECODE)
    assert body == gz and hd["headers"]["content-encoding"] == ["gzip"] and "decode=datatypes" in hd["info"], hd
    M.free()
    os.environ.pop("WFNP_DT_MISSING")
    assert DL.wfnp_dt_live() == 0, DL.wfnp_dt_live()
    print(f"datatypes through the transport: Accept-Encoding: {accept} (identity with none installed, over HTTP/1.1 "
          "and HTTP/2), bodies undone over HTTP and HTTPS with decode=<datatype>, two codings in a row, HEAD, an empty "
          "body; damaged and cut short give error 8; an unknown coding or a missing datatype leaves the body as sent "
          "(decode=datatypes); 3 MB through a 16 KB window over HTTP/1.1 and HTTP/2; every object disposed")
else:
    print(f"datatypes' glue: skipped, no opendatatypes checkout at {ODT} (set ODT)")

# ---- an independent HTTP/2 server: Go's net/http, when Go is here ----------------------------------------

GO = shutil.which("go")
if GO:
    gdir = os.path.join(TMP, "goh2")
    os.makedirs(gdir)
    with open(os.path.join(gdir, "go.mod"), "w") as f:
        f.write("module goh2\n\ngo 1.21\n")
    with open(os.path.join(gdir, "main.go"), "w") as f:
        f.write("""package main
import ("crypto/md5";"fmt";"io";"net";"net/http";"os")
func main() {
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	fmt.Println(ln.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		w.Header().Set("X-Proto", r.Proto)
		w.Header().Add("Set-Cookie", "g=1")
		w.Header().Add("Set-Cookie", "h=2")
		fmt.Fprintf(w, "%s %s %d %x %s", r.Proto, r.URL.Path, len(b), md5.Sum(b), r.Header.Get("X-Long"))
	})
	http.ServeTLS(ln, nil, os.Args[1], os.Args[2])
}
""")
    env = dict(os.environ, GOCACHE=os.path.join(TMP, "gocache"), GOPATH=os.path.join(TMP, "gopath"), GOFLAGS="-mod=mod",
               GOTOOLCHAIN="local")
    b = subprocess.run([GO, "build", "-o", "goh2", "."], cwd=gdir, env=env, capture_output=True, text=True, timeout=300)
    if b.returncode == 0:
        gp = subprocess.Popen([os.path.join(gdir, "goh2"), f"{TMP}/srv.pem", f"{TMP}/srv.key"], stdout=subprocess.PIPE, text=True)
        gport = int(gp.stdout.readline())
        long = "v" * 5000
        hs = [F.open(f"POST https://localhost:{gport}/p{i}\nX-Long: {long}\n", BODY, 30000, 0, bytes([i]) * 30000) for i in range(8)]
        for i, h in enumerate(hs):
            hd = F.head(h)
            body, _ = F.read_all(h)
            want = f"HTTP/2.0 /p{i} 30000 {hashlib.md5(bytes([i]) * 30000).hexdigest()} {long}".encode()
            assert hd["status"] == 200 and hd["version"] == 20 and body == want, (hd, body[:80])
            assert hd["headers"]["set-cookie"] == ["g=1", "h=2"], hd
            F.close(h)
        gp.kill()
        print("Go's net/http HTTP/2 server: 8 uploads of 30 KB side by side, a 5 KB header, cookies kept")
    else:
        print("Go's HTTP/2 server: skipped, it would not build here")
else:
    print("Go's HTTP/2 server: skipped, no Go here")

assert F.stats()[0] == 0, F.stats()
F.free()
D.free()
shutil.rmtree(TMP, ignore_errors=True)
print("all passed")
