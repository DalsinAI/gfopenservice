"""Checks opentls_key.c against Python's cryptography. MIT, Copyright (c) 2026 AmiCradle Team."""
import hashlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, x25519, utils
import opentls_key as otk

spki = lambda k: k.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
msg = b"GFOpenBrowser key offload probe"
d256 = hashlib.sha256(msg).digest()

def verify(arg, md, key_der, digest, sig, salt=0):
    return otk.call(1, arg, [md, salt, 0, 0], 0, [key_der, digest, sig, None], [0] * 4)[:2]

r = rsa.generate_private_key(public_exponent=65537, key_size=2048)
sig = r.sign(msg, padding.PKCS1v15(), hashes.SHA256())
assert verify(1, 2, spki(r), d256, sig) == (0, 1)
assert verify(1, 2, spki(r), d256, sig[:-1] + bytes([sig[-1] ^ 1])) == (0, 0)
pss = r.sign(msg, padding.PSS(padding.MGF1(hashes.SHA256()), 32), hashes.SHA256())
assert verify(2, 2, spki(r), d256, pss) == (0, 1)
e = ec.generate_private_key(ec.SECP256R1())
esig = e.sign(msg, ec.ECDSA(hashes.SHA256()))
assert verify(3, 2, spki(e), d256, esig) == (0, 1)
assert verify(3, 2, spki(e), hashlib.sha256(b"x").digest(), esig) == (0, 0)
print("verify: RSA PKCS#1, RSA-PSS, ECDSA ok")

for curve, name, mk in ((1, "X25519", None), (2, "P-256", ec.SECP256R1()), (3, "P-384", ec.SECP384R1())):
    st, publen, privlen, w = otk.call(2, curve, [0] * 4, 3, [None] * 4, [66, 133, 0, 0])
    assert st == 0, (name, st)
    priv, pub = w[0], w[1]
    if curve == 1:
        peer = x25519.X25519PrivateKey.generate()
        peer_pub = peer.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        expect = peer.exchange(x25519.X25519PublicKey.from_public_bytes(pub))
    else:
        peer = ec.generate_private_key(mk)
        peer_pub = peer.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        expect = peer.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(mk, pub))
    st, n, _, w = otk.call(3, curve, [0] * 4, 4, [priv, peer_pub, None, None], [0, 0, 66, 0])
    assert st == 0 and w[2] == expect, (name, st)
    print(f"{name}: keygen {len(priv)}+{len(pub)} bytes, derive matches")
