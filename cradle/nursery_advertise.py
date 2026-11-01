#!/usr/bin/env python3
"""Advertise a Cradle's services to Amigas on the LAN (the nursery).

Answers mDNS questions for _amicradle._tcp.local with this Cradle's
instance: PTR, SRV, TXT (svc=, fp=, name=) and A records. Standard library
only; shares UDP 5353 with avahi and browsers on the same machine.

  nursery_advertise.py --services opentls.key/1,media.decode/1

MIT, Copyright (c) 2026 AmigaChrome Team.
"""
import argparse
import socket
import struct
import sys
import time

GROUP = "224.0.0.251"
PORT = 5353
SERVICE = "_amicradle._tcp.local"
TYPE_A, TYPE_PTR, TYPE_TXT, TYPE_SRV, TYPE_ANY = 1, 12, 16, 33, 255
CLASS_IN, CACHE_FLUSH = 1, 0x8000


def put_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        raw = label.encode()
        out += bytes([len(raw)]) + raw
    return out + b"\0"


def get_name(msg, at):
    """Returns (name, offset after the name) for a possibly compressed name."""
    labels, end, hops = [], None, 0
    while True:
        if at >= len(msg) or hops > 64:
            raise ValueError("bad name")
        n = msg[at]
        if n & 0xC0 == 0xC0:
            if end is None:
                end = at + 2
            at = (n & 0x3F) << 8 | msg[at + 1]
            hops += 1
            continue
        at += 1
        if n == 0:
            break
        labels.append(msg[at:at + n].decode(errors="replace"))
        at += n
    return ".".join(labels), end if end is not None else at


def record(name, rtype, rdata, ttl=120, flush=False):
    rclass = CLASS_IN | (CACHE_FLUSH if flush else 0)
    return put_name(name) + struct.pack(">HHIH", rtype, rclass, ttl, len(rdata)) + rdata


def txt(items):
    out = b""
    for item in items:
        raw = item.encode()[:255]
        out += bytes([len(raw)]) + raw
    return out


def local_address():
    """The address other machines reach us on (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    finally:
        s.close()


class Advert:
    def __init__(self, args):
        self.host = (args.host or socket.gethostname().split(".")[0]) + ".local"
        self.instance = f"{args.instance or socket.gethostname().split('.')[0]}.{SERVICE}"
        self.port = args.port
        self.address = args.address or local_address()
        self.txt = [f"svc={args.services}", f"name={args.name or socket.gethostname()}", "v=1"]
        if args.fingerprint:
            self.txt.append(f"fp={args.fingerprint}")

    def answer(self, qid=0, question=None):
        """A response: the PTR answer, with SRV, TXT and A as additionals."""
        answers = [record(SERVICE, TYPE_PTR, put_name(self.instance), ttl=4500)]
        extra = [
            record(self.instance, TYPE_SRV, struct.pack(">HHH", 0, 0, self.port) + put_name(self.host), flush=True),
            record(self.instance, TYPE_TXT, txt(self.txt), ttl=4500, flush=True),
            record(self.host, TYPE_A, socket.inet_aton(self.address), flush=True),
        ]
        head = struct.pack(">HHHHHH", qid, 0x8400, 1 if question else 0, len(answers), 0, len(extra))
        return head + (question or b"") + b"".join(answers) + b"".join(extra)

    def wanted(self, msg):
        """The question section bytes when msg asks for our service, else None."""
        if len(msg) < 12:
            return None
        qid, flags, qd = struct.unpack(">HHH", msg[:6])
        if flags & 0x8000:
            return None
        at = 12
        for _ in range(qd):
            start = at
            name, at = get_name(msg, at)
            qtype, _qclass = struct.unpack(">HH", msg[at:at + 4])
            at += 4
            if qtype in (TYPE_PTR, TYPE_ANY) and name.lower() == SERVICE.lower():
                return put_name(SERVICE) + struct.pack(">HH", TYPE_PTR, CLASS_IN)
        return None


def add_arguments(p):
    p.add_argument("--services", required=True, help="comma-separated, e.g. opentls.key/1,media.decode/1")
    p.add_argument("--name", help="what Amigas call this Cradle")
    p.add_argument("--instance", help="the DNS-SD instance label (default: host name)")
    p.add_argument("--host", help="the .local host name (default: host name)")
    p.add_argument("--address", help="the IPv4 address to advertise")
    p.add_argument("--port", type=int, default=7420, help="the peer port (APPLIANCE_DESIGN.md)")
    p.add_argument("--fingerprint", help="pairing fingerprint for the fp= key")


def advertise(advert):
    """Answers mDNS questions for advert, for ever."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", PORT))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 socket.inet_aton(GROUP) + socket.inet_aton(advert.address))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(advert.address))

    print(f"nursery: {advert.instance} at {advert.address}:{advert.port}, {' '.join(advert.txt)}", flush=True)
    for _ in range(2):                                   # announce (RFC 6762, 8.3)
        s.sendto(advert.answer(), (GROUP, PORT))
        time.sleep(1)
    while True:
        msg, (addr, port) = s.recvfrom(9000)
        try:
            question = advert.wanted(msg)
        except (ValueError, struct.error):
            continue
        if question is None:
            continue
        if port == PORT:
            s.sendto(advert.answer(), (GROUP, PORT))
        else:                                            # legacy unicast (RFC 6762, 6.7)
            s.sendto(advert.answer(struct.unpack(">H", msg[:2])[0], question), (addr, port))
        print(f"nursery: answered {addr}:{port}", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(p)
    advertise(Advert(p.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
