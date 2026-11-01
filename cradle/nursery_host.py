#!/usr/bin/env python3
"""A Cradle in the nursery: services for Amigas on the LAN.

Advertises over mDNS (nursery_advertise.py) and serves gfopenservice.device's
LAN frames on TCP: each request is the services card's 64-byte entry, then
the buffers the service reads; each answer is the 16-byte completion, then
for each buffer the service writes a u32 byte count and the bytes.

Services here: the directory (OPEN, CLOSE, LIST, CANCEL), echo/1, which
copies buffer 0 into buffer 1 as the emulator's test service does,
opentls.key/1 (host/opentls_key.c) when host/libopentlskey.so is built, and
media.decode/1 (host/media_decode.c) when host/libmediadecode.so is built,
media.cdxl/1 (host/cdxl.c) when it is built with FFmpeg, and doc.render/1
(host/doc_render.c) when host/libdocrender.so is.

  nursery_host.py --fingerprint <fp>

MIT, Copyright (c) 2026 AmiCradle Team.
"""
import argparse
import socket
import socketserver
import struct
import sys
import threading

import nursery_advertise
import nursery_link

OK, NOSERVICE, BADREQUEST, CANCELLED, TOOSMALL, HOSTERROR = 0, -1, -2, -3, -4, -5
DIR_OPEN, DIR_CLOSE, DIR_LIST, OP_CANCEL = 1, 2, 3, 0xFFFF


def echo(op, arg, bufs, extra):
    """echo/1: buffer 0 into buffer 1."""
    data = bufs[0] or b""
    return OK, len(data), 0, {1: data}


SERVICES = {"echo/1": echo}

try:                                                  # host/libopentlskey.so, when built
    sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "host"))
    import opentls_key

    def opentls(op, arg, bufs, extra, flags=0, lengths=(0, 0, 0, 0)):
        status, result, aux, written = opentls_key.call(op, arg, extra, flags, bufs, lengths)
        return status, result, aux, written

    SERVICES["opentls.key/1"] = opentls
except OSError:
    pass

try:                                                  # host/libmediadecode.so, when built
    import media_decode

    def media(op, arg, bufs, extra, flags=0, lengths=(0, 0, 0, 0)):
        return media_decode.call(op, arg, extra, flags, bufs, lengths)

    SERVICES["media.decode/1"] = media

    if media_decode.has_cdxl():
        def cdxl(op, arg, bufs, extra, flags=0, lengths=(0, 0, 0, 0)):
            return media_decode.cdxl_call(op, arg, extra, flags, bufs, lengths)

        SERVICES["media.cdxl/1"] = cdxl
except OSError:
    pass

try:                                                  # host/libdocrender.so, when built
    import doc_render

    def document(op, arg, bufs, extra, flags=0, lengths=(0, 0, 0, 0)):
        return doc_render.call(op, arg, extra, flags, bufs, lengths)

    SERVICES["doc.render/1"] = document
except OSError:
    pass

# Services whose answers fill buffers the Amiga sized: they get the flags and room.
SIZED = {"opentls.key/1", "media.decode/1", "media.cdxl/1", "doc.render/1"}


class Connection(socketserver.BaseRequestHandler):
    link = None          # a sealed session (nursery_link.Link), else plain
    early = b""          # bytes read to tell pairing, sessions and plain apart

    def raw_all(self, n):
        out, self.early = self.early[:n], self.early[n:]
        while len(out) < n:
            chunk = self.request.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    def recv_all(self, n):
        return self.link.recv_all(n) if self.link else self.raw_all(n)

    def reply(self, rid, status, result, aux, flags, lengths, written):
        out = struct.pack(">IiII", rid, status, result, aux)
        for i in range(4):
            if lengths[i] and flags & (1 << i):
                data = written.get(i, b"")[:lengths[i]]
                out += struct.pack(">I", len(data)) + data
        (self.link or self.request).sendall(out)

    def start(self, peer):
        """Pairing, a sealed session, or (with --allow-plain) plain frames."""
        first = self.raw_all(4)
        if first == nursery_link.PAIR:
            def confirm(code, name):
                if not self.server.allow_pairing:
                    print(f"host: {peer} ({name}) asked to pair; start with --pair to allow it", flush=True)
                    return False
                print(f"host: {peer} ({name}) pairing, code {code // 1000:03d} {code % 1000:03d}: accepted (--pair)", flush=True)
                return True
            name = nursery_link.pair(self.request, self.raw_all, confirm)
            print(f"host: {peer} pairing {'done with ' + name if name else 'refused'}", flush=True)
            return False
        if first == nursery_link.SESSION:
            self.link = nursery_link.session(self.request, self.raw_all)
            if not self.link:
                print(f"host: {peer} not paired; refused", flush=True)
            return self.link is not None
        if not self.server.allow_plain:
            print(f"host: {peer} sent plain frames; start with --allow-plain to allow them", flush=True)
            return False
        self.early = first + self.early
        return True

    def handle(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer = self.client_address[0]
        opened = {}                                   # handle -> service name
        print(f"host: {peer} connected", flush=True)
        try:
            if not self.start(peer):
                return
            while True:
                entry = self.recv_all(64)
                rid, service, op, flags, arg = struct.unpack(">IHHII", entry[:16])
                pairs = struct.unpack(">8I", entry[16:48])
                extra = struct.unpack(">4I", entry[48:64])
                lengths = [pairs[1], pairs[3], pairs[5], pairs[7]]
                bufs = [self.recv_all(lengths[i]) if lengths[i] and not flags & (1 << i) else None
                        for i in range(4)]
                written = {}
                if op == OP_CANCEL:
                    status, result, aux = OK, 0, 0    # requests here finish at once
                elif service == 0 and op == DIR_OPEN:
                    name = (bufs[0] or b"").split(b"\0")[0].decode(errors="replace")
                    if name in SERVICES:
                        handle = max(opened, default=0) + 1
                        opened[handle] = name
                        status, result, aux = OK, handle, 0
                    else:
                        status, result, aux = NOSERVICE, 0, 0
                    print(f"host: {peer} open {name!r} -> {status} {result}", flush=True)
                elif service == 0 and op == DIR_CLOSE:
                    opened.pop(arg, None)
                    status, result, aux = OK, 0, 0
                elif service == 0 and op == DIR_LIST:
                    names = b"".join(n.encode() + b"\0" for n in SERVICES)
                    written[0] = names
                    status, result, aux = OK, min(len(names), lengths[0]), 0
                elif service in opened and opened[service] in SIZED:
                    status, result, aux, written = SERVICES[opened[service]](op, arg, bufs, extra, flags, lengths)
                elif service in opened:
                    status, result, aux, written = SERVICES[opened[service]](op, arg, bufs, extra)
                else:
                    status, result, aux = BADREQUEST, 0, 0
                self.reply(rid, status, result, aux, flags, lengths, written)
        except (EOFError, ConnectionError, ValueError, nursery_link.InvalidTag):
            print(f"host: {peer} gone", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    nursery_advertise.add_arguments(p)
    p.set_defaults(services=None)
    for action in p._actions:
        if action.dest == "services":
            action.required = False
    p.add_argument("--pair", action="store_true", help="accept pairing (the code shows here)")
    p.add_argument("--allow-plain", action="store_true", help="also take unsealed frames (tests)")
    args = p.parse_args()
    args.services = args.services or ",".join(SERVICES)
    args.fingerprint = args.fingerprint or nursery_link.cradle_id()
    advert = nursery_advertise.Advert(args)
    threading.Thread(target=nursery_advertise.advertise, args=(advert,), daemon=True).start()
    with Server(("", args.port), Connection) as server:
        server.allow_pairing, server.allow_plain = args.pair, args.allow_plain
        print(f"host: serving {args.services} on TCP {args.port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
