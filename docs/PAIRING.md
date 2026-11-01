# Pairing and the sealed LAN link

An Amiga uses a Cradle on the LAN only after pairing with it once. Pairing
agrees a key; every session after that is sealed with it, so nobody else on
the LAN can read the work (TLS key exchanges, pictures) or answer in the
Cradle's place.

## Pairing: `Nursery PAIR=<Cradle>`

Both sides make a fresh X25519 key. The Cradle commits to its key before
seeing the Amiga's, then both show a six-digit code made from both keys and
both nonces. The person checks the codes match and says so on each side
(on the Amiga by answering `y`; on the test Cradle by starting it with
`--pair`, which accepts and logs the code). A machine in the middle would
have to make both codes match without knowing them in advance: one chance
in a million, as Bluetooth's numeric comparison.

On TCP to the Cradle's port (the one mDNS gives):

| From | Bytes |
| --- | --- |
| Amiga | `OSP1`, the Amiga's id (16), its name (32, NUL padded) |
| Cradle | `OSP1`, the Cradle's id (16, its mDNS `fp=` in hex), SHA-256(C_pub ‖ nc) (32) |
| Amiga | A_pub (32), na (16) |
| Cradle | C_pub (32), nc (16) |
| both | one byte: 1 the codes matched and were accepted, else 0 |

The code is the first four bytes of SHA-256("openservice sas" ‖ A_pub ‖ C_pub
‖ na ‖ nc), big-endian, modulo 1,000,000. With both bytes 1 the key is
HKDF-SHA256(X25519 secret, salt na ‖ nc, info "openservice pair v1", 32 bytes).

The Amiga keeps `fp key name` in `ENV:OpenService/Paired` and
`ENVARC:OpenService/Paired`, and its id (made at the first pairing) in
`ENV:`/`ENVARC:OpenService/ID`. The Cradle keeps the Amiga's id and key in
`paired.json` (`$OPENSERVICE_STATE`, else `~/.config/openservice`, mode
600). A line with a fingerprint alone still allows unsealed frames to a test
Cradle started with `--allow-plain`; a real Cradle refuses them.

## Sessions

| From | Bytes |
| --- | --- |
| Amiga | `OSE1`, its id (16), na (16) |
| Cradle | `OSE1`, nc (16); or `OSE0` and close, for an Amiga it has not paired |

Keys: HKDF-SHA256(the pairing key, salt na ‖ nc, info "openservice session
v1", 64 bytes): the first 32 seal the Amiga's records, the last 32 the
Cradle's. Each record is a big-endian u32 length (of what follows, the tag
included), then the ChaCha20-Poly1305 ciphertext and its 16-byte tag; the
nonce is four zero bytes and a 64-bit big-endian count of records in that
direction; the length is the additional data. Inside are the same frames
as before (`SERVICES_CARD.md`'s entry and buffers; the completion and the
written buffers). A record that does not open closes the connection.

The `OSE1` answer proves nothing: any host on the LAN can send it, and the
`fp=` a Cradle is found by is public. So the device's first sealed record
on a new session is a directory LIST (id 0, no buffers), and it uses the
connection only when the sealed completion for id 0 comes back and opens
with the key, within 10 s of the connection's start. Otherwise it closes
it and tries the Cradle's next address: mDNS keeps every A record that
came for the Cradle's SRV target (up to 8), not the last.

## On the Amiga

The device and `Nursery` carry their own SHA-256, HMAC, HKDF,
ChaCha20-Poly1305 and X25519 (`src/oscrypto.c`, checked against Python's
`cryptography`), so nothing else needs installing. Random bytes come from
the E clock, the scheduler's counters and the time, stirred with SHA-256;
pairing keys also take 64 frames of timing jitter.

Measured on the bench (AC090, before its FPU and clock fixes reached it):
making the pairing key takes about 11 s; a sealed echo about 9 ms against
2.2 ms unsealed; sealing 4 KB about 44 ms, most of it Poly1305.
