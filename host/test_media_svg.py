"""Checks SVG in media_decode.c: its own size, the size asked, preserveAspectRatio,
SVGZ, and the plain picture path's largest size. Needs librsvg-2.so.2 at run time.
MIT, Copyright (c) 2026 AmiCradle Team."""
import gzip
import struct
import time
import media_decode as md

PROBE, DECODE, EXACT = 1, 2, 1
SVG = 0x53564720


def hint(ext):
    return int.from_bytes(ext.upper().ljust(4).encode(), "big")


def call(op, data, w=0, h=0, flags=EXACT, ext="", room=0):
    st, a, b, out = md.call(op, 0, [w, h, hint(ext) if ext else 0, flags], 2, [data, None, None, None],
                            [0, room or 4096 * 4096 * 4, 0, 0])
    return st, a, b, out[1]


def px(out, w, x, y):
    return tuple(out[(y * w + x) * 4:(y * w + x) * 4 + 4])


def probe(data, w=0, h=0, flags=EXACT, ext=""):
    st, a, b, info = call(PROBE, data, w, h, flags, ext, room=24)
    assert st == 0, st
    kind, fmt, fl, frames, ow, oh = struct.unpack(">6I", info)
    assert (kind, fmt, fl & 1, frames) == (1, SVG, 1, 1)
    return a, b, ow, oh


RED = b'<svg xmlns="http://www.w3.org/2000/svg" %s><rect x="0" y="0" width="16" height="16" fill="#f00"/></svg>'
ICON = RED % b'width="16" height="16" viewBox="0 0 16 16"'
# Its own size: width and height, else the viewBox, else 300 x 150.
assert probe(ICON) == (16, 16, 16, 16)
assert probe(RED % b'viewBox="0 0 48 24"')[:2] == (48, 24)
assert probe(RED % b'')[:2] == (300, 150)
assert probe(RED % b'width="2in" viewBox="0 0 10 20"')[:2] == (192, 384)
assert probe(RED % b'width="12pt" height="1cm"')[:2] == (16, 38)
assert probe(RED % b'width="100%" height="100%" viewBox="0 0 32 8"')[:2] == (32, 8)
print("own size: width/height, viewBox, units and 300x150 default")

# The size asked: both, one (aspect kept), none; capped at 4096.
assert probe(ICON, 64, 64)[2:] == (64, 64)
assert probe(ICON, 40, 0)[2:] == (40, 40)
assert probe(RED % b'viewBox="0 0 48 24"', 0, 12)[2:] == (24, 12)
assert probe(ICON, 9000, 9000)[2:] == (4096, 4096)
assert probe(ICON, 8192, 4096)[2:] == (4096, 2048)
# Without MD_EXACT the sizes are the largest, as for any picture: never larger.
assert probe(ICON, 4096, 4096, flags=0)[2:] == (16, 16)
assert probe(RED % b'viewBox="0 0 400 200"', 100, 100, flags=0)[2:] == (100, 50)
print("asked size: exact, one side, capped; largest without MD_EXACT")

# A 16x16 icon drawn at 64x64: solid red, opaque, in milliseconds.
t = time.time()
st, w, h, out = call(DECODE, ICON, 64, 64)
ms = (time.time() - t) * 1000
assert (st, w, h, len(out)) == (0, 64, 64, 64 * 64 * 4)
assert px(out, 64, 0, 0) == px(out, 64, 63, 63) == px(out, 64, 32, 32) == (255, 255, 0, 0), px(out, 64, 0, 0)
print(f"icon 16x16 drawn at 64x64 in {ms:.1f} ms")

# preserveAspectRatio: a square drawn into 64x32 sits in the middle (meet),
# fills it stretched (none), or sits at the left (xMinYMid).
SQ = b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" %s><rect width="16" height="16" fill="#00f"/></svg>'
st, w, h, out = call(DECODE, SQ % b'', 64, 32)
assert (w, h) == (64, 32)
assert px(out, 64, 32, 16) == (255, 0, 0, 255) and px(out, 64, 2, 16)[0] == 0 and px(out, 64, 61, 16)[0] == 0
st, w, h, out = call(DECODE, SQ % b'preserveAspectRatio="none"', 64, 32)
assert px(out, 64, 2, 16) == px(out, 64, 61, 16) == (255, 0, 0, 255)
st, w, h, out = call(DECODE, SQ % b'preserveAspectRatio="xMinYMid meet"', 64, 32)
assert px(out, 64, 2, 16) == (255, 0, 0, 255) and px(out, 64, 61, 16)[0] == 0
# No viewBox: scaled as a whole and centred.
st, w, h, out = call(DECODE, b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#0f0"/></svg>', 64, 32)
assert px(out, 64, 32, 16) == (255, 0, 255, 0) and px(out, 64, 2, 16)[0] == 0
print("preserveAspectRatio: meet centred, none stretched, xMin left; no viewBox centred")

# Half-transparent colour comes back straight, not premultiplied.
st, w, h, out = call(DECODE, b'<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4" fill="#ff8000" fill-opacity="0.5"/></svg>')
a, r, g, b = px(out, 4, 1, 1)
assert abs(a - 128) <= 1 and r == 255 and abs(g - 128) <= 2 and b == 0, (a, r, g, b)
print("alpha straight A,R,G,B")

# SVGZ, by name; an XML prolog and comments before <svg.
assert probe(gzip.compress(ICON), ext="svgz")[:2] == (16, 16)
assert probe(b'<?xml version="1.0"?>\n<!-- made by hand -->\n' + ICON)[:2] == (16, 16)
assert call(DECODE, ICON, 16, 16, room=100)[0] == -4
assert call(PROBE, b'<svg xmlns="http://www.w3.org/2000/svg"><rect', ext="svg", room=24)[0] == -2
print("SVGZ, prolog, too-small buffer and broken SVG")
