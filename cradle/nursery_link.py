"""The Nursery's LAN link, Cradle side (docs/PAIRING.md): pairing with a
six-digit code both sides show, and sessions sealed with ChaCha20-Poly1305
under the key pairing agreed.

MIT, Copyright (c) 2026 AmiCradle Team.
"""
import hashlib
import json
import os
import struct

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.exceptions import InvalidTag  # noqa: F401 (callers catch it)
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

PAIR, SESSION, REFUSED = b"OSP1", b"OSE1", b"OSE0"
MAX_RECORD = 16 << 20


def state_dir():
    d = os.environ.get("OPENSERVICE_STATE") or os.path.expanduser("~/.config/openservice")
    os.makedirs(d, exist_ok=True)
    return d


def _load(name, default):
    try:
        with open(os.path.join(state_dir(), name)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def _save(name, value):
    path = os.path.join(state_dir(), name)
    with open(path + ".tmp", "w") as f:
        json.dump(value, f, indent=1)
    os.chmod(path + ".tmp", 0o600)
    os.replace(path + ".tmp", path)


def cradle_id():
    """This Cradle's identity: 16 random bytes, the mDNS fp= value in hex."""
    state = _load("cradle.json", {})
    if "id" not in state:
        state["id"] = os.urandom(16).hex()
        _save("cradle.json", state)
    return state["id"]


def hkdf(secret, salt, info, length):
    return HKDF(hashes.SHA256(), length, salt, info.encode()).derive(secret)


def raw_public(key):
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def sas(a_pub, c_pub, na, nc):
    """The six-digit code both sides show."""
    digest = hashlib.sha256(b"openservice sas" + a_pub + c_pub + na + nc).digest()
    return struct.unpack(">I", digest[:4])[0] % 1000000


def pair(sock, recv_all, confirm):
    """Pairing, after the client's "OSP1". confirm(code, name) says whether
    the person at the Cradle accepted. Returns the Amiga's name or None."""
    amiga_id = recv_all(16)
    name = recv_all(32).split(b"\0")[0].decode(errors="replace")
    ours = X25519PrivateKey.generate()
    c_pub, nc = raw_public(ours), os.urandom(16)
    sock.sendall(PAIR + bytes.fromhex(cradle_id()) + hashlib.sha256(c_pub + nc).digest())
    a_pub, na = recv_all(32), recv_all(16)
    sock.sendall(c_pub + nc)                       # opened after the Amiga committed to its key
    secret = ours.exchange(X25519PublicKey.from_public_bytes(a_pub))
    code = sas(a_pub, c_pub, na, nc)
    ok = bool(confirm(code, name))
    sock.sendall(b"\1" if ok else b"\0")
    theirs = recv_all(1) == b"\1"
    if not (ok and theirs):
        return None
    paired = _load("paired.json", {})
    paired[amiga_id.hex()] = {"psk": hkdf(secret, na + nc, "openservice pair v1", 32).hex(), "name": name}
    _save("paired.json", paired)
    return name


class Link:
    """A sealed session: records of u32 length (the length of what follows,
    tag included), then the ciphertext and its 16-byte tag. The nonce is
    four zero bytes and a 64-bit counter per direction; the length is the
    additional data."""

    def __init__(self, sock, psk, na, nc):
        keys = hkdf(psk, na + nc, "openservice session v1", 64)
        self.sock = sock
        self.rx, self.tx = ChaCha20Poly1305(keys[:32]), ChaCha20Poly1305(keys[32:])
        self.rx_n = self.tx_n = 0
        self.pending = b""

    def _raw(self, n):
        out = b""
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    def recv_all(self, n):
        while len(self.pending) < n:
            head = self._raw(4)
            length = struct.unpack(">I", head)[0]
            if length < 16 or length > MAX_RECORD:
                raise ConnectionError("bad record")
            nonce = b"\0\0\0\0" + struct.pack(">Q", self.rx_n)
            self.rx_n += 1
            self.pending += self.rx.decrypt(nonce, self._raw(length), head)   # raises on a bad tag
        out, self.pending = self.pending[:n], self.pending[n:]
        return out

    def sendall(self, data):
        nonce = b"\0\0\0\0" + struct.pack(">Q", self.tx_n)
        self.tx_n += 1
        head = struct.pack(">I", len(data) + 16)
        self.sock.sendall(head + self.tx.encrypt(nonce, data, head))


def session(sock, recv_all):
    """A session, after the client's "OSE1". Returns a Link, or None (and
    "OSE0" sent) for an Amiga that has not paired."""
    amiga_id, na = recv_all(16), recv_all(16)
    entry = _load("paired.json", {}).get(amiga_id.hex())
    if not entry:
        sock.sendall(REFUSED)
        return None
    nc = os.urandom(16)
    sock.sendall(SESSION + nc)
    return Link(sock, bytes.fromhex(entry["psk"]), na, nc)
