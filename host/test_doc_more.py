"""Checks doc.render/1's formats past office documents: PostScript and EPS
through Ghostscript, EPUB and Markdown through pandoc, CSV, HTML and text
through LibreOffice, the text ones named by the extension hint.
MIT, Copyright (c) 2026 AmiCradle Team."""
import os
import struct
import subprocess
import tempfile
import zipfile

PROBE, RENDER, TEXT = 1, 2, 3


def hint(ext):
    return struct.unpack(">I", (ext.upper() + "    ")[:4].encode())[0] if ext else 0


def call(op, data, ext="", maxw=600, room=0, page=0):
    return dr.call(op, page, [maxw, 0, hint(ext), 0], 2, [data, None, None, None], [0, room, 0, 0])


PS = b"""%!PS-Adobe-3.0
/Helvetica findfont 24 scalefont setfont
72 700 moveto (GFOpenAmiga page one) show showpage
72 700 moveto (Second page text) show showpage
"""
EPS = b"""%!PS-Adobe-3.0 EPSF-3.0
%%BoundingBox: 0 0 200 100
/Helvetica findfont 20 scalefont setfont
10 40 moveto (GFOpenAmiga page one) show
"""


def epub(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        z.writestr("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" '
                   'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile '
                   'full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        z.writestr("book.opf", '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
                   'unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
                   '<dc:identifier id="id">openamiga-test</dc:identifier><dc:title>Test</dc:title>'
                   '<dc:language>en</dc:language></metadata><manifest><item id="c1" href="c1.xhtml" '
                   'media-type="application/xhtml+xml"/></manifest><spine><itemref idref="c1"/></spine></package>')
        z.writestr("c1.xhtml", '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>c</title>'
                   '</head><body><h1>GFOpenAmiga page one</h1><p>Hello from the Cradle.</p></body></html>')


with tempfile.TemporaryDirectory() as tmp:
    os.environ["OPENSERVICE_CACHE"] = os.path.join(tmp, "cache")
    import doc_render as dr
    epub(os.path.join(tmp, "b.epub"))
    cases = [
        ("ps", PS, "", "PS  ", 2),
        ("eps", EPS, "", "EPS ", 1),
        ("epub", open(os.path.join(tmp, "b.epub"), "rb").read(), "", "EPUB", 1),
        ("md", b"# GFOpenAmiga page one\n\nHello from the *Cradle*.\n", "md", "MD  ", 1),
        ("csv", b"Name,Count\nOpenAmiga page one,1200\nCradle,1\n", "csv", "CSV ", 1),
        ("html", b"<html><body><h1>GFOpenAmiga page one</h1></body></html>", "html", "HTML", 1),
        ("txt", b"GFOpenAmiga page one\nPlain text.\n", "txt", "TXT ", 1),
    ]
    for name, data, ext, fmt, pages in cases:
        st, n, _, out = call(PROBE, data, ext, room=24)
        assert st == 0, (name, st)
        kind, f, flags, n2, ow, oh = struct.unpack(">6I", out[1])
        assert kind == 4 and f == struct.unpack(">I", fmt.encode())[0], (name, kind, struct.pack(">I", f))
        assert n == pages, (name, n)
        st, w, h, out = call(RENDER, data, ext, room=ow * oh * 4)
        assert st == 0 and (w, h) == (ow, oh), (name, st)
        dark = sum(1 for i in range(1, len(out[1]), 4 * 7) if out[1][i] < 128)
        assert dark > 20, (name, dark)
        st, size, _, out = call(TEXT, data, ext, room=65536)
        assert st == 0 and b"GFOpenAmiga page one" in out[1], (name, st, out[1][:80])
        print(f"{name}: {fmt} {n} page(s) at {ow}x{oh}, {dark} dark pixels sampled, text ok")
    assert call(PROBE, b"Name,Count\n1,2\n", "", room=24)[0] == -2      # CSV needs its hint
    print("text without a hint refused")
