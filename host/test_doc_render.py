"""Checks doc_render.c with documents LibreOffice itself writes: a two-page
text as DOCX, ODT, RTF and DOC, and a table as XLSX.
MIT, Copyright (c) 2026 AmigaChrome Team."""
import os
import struct
import subprocess
import tempfile
import time
import doc_render as dr

PROBE, RENDER, TEXT = 1, 2, 3


def call(op, data, maxw=0, maxh=0, room=0, page=0):
    return dr.call(op, page, [maxw, maxh, 0, 0], 2, [data, None, None, None], [0, room, 0, 0])


with tempfile.TemporaryDirectory() as tmp:
    os.environ["OPENSERVICE_CACHE"] = os.path.join(tmp, "cache")
    html = os.path.join(tmp, "two.html")
    open(html, "w").write("<html><body><h1>GFOpenAmiga page one</h1><p>Hello from the Cradle.</p>"
                          "<p style='page-break-before: always'>Second page text</p></body></html>")
    csv = os.path.join(tmp, "table.csv")
    open(csv, "w").write("Name,Count\nAmiga,1200\nCradle,1\n")
    made = {}
    for ext, filt, src in (("docx", "docx:MS Word 2007 XML", html), ("odt", "odt:writer8", html),
                           ("rtf", "rtf:Rich Text Format", html), ("doc", "doc:MS Word 97", html),
                           ("xlsx", "xlsx:Calc MS Excel 2007 XML", csv)):
        subprocess.run(["soffice", "--headless", "--convert-to", filt, "--outdir", tmp, src],
                       check=True, capture_output=True)
        made[ext] = os.path.join(tmp, os.path.splitext(os.path.basename(src))[0] + "." + ext)
        made[ext] = open(made[ext], "rb").read()
    for ext, data in made.items():
        t0 = time.time()
        st, pages, _, out = call(PROBE, data, 600, 0, room=24)
        kind, fmt, flags, n, ow, oh = struct.unpack(">6I", out[1])
        assert st == 0 and kind == 4 and n == pages and ow == 600 and oh > 600, (ext, st, kind, n, ow, oh)
        t1 = time.time()
        st, w, h, out = call(RENDER, data, 600, 0, room=ow * oh * 4)
        assert st == 0 and (w, h) == (ow, oh) and len(out[1]) == w * h * 4, (ext, st, w, h)
        px = out[1]
        dark = sum(1 for i in range(0, len(px), 4) if px[i + 1] < 128)
        assert px[0] == 255 and 50 < dark < w * h // 4, (ext, dark)      # some text, mostly white
        st, nbytes, _, out = call(TEXT, data, room=65536)
        txt = out[1].decode("latin-1")
        want = "GFOpenAmiga" if ext != "xlsx" else "Cradle"
        assert st == 0 and want in txt, (ext, st, txt[:80])
        t2 = time.time()
        if ext != "xlsx":
            assert pages == 2, (ext, pages)
            assert call(RENDER, data, 600, 0, room=ow * oh * 4, page=1)[0] == 0
            assert call(RENDER, data, 600, 0, room=ow * oh * 4, page=2)[0] == -2
        print(f"{ext}: {pages} page(s) at {ow}x{oh}, {dark} dark pixels, text ok; "
              f"first probe {t1 - t0:.1f} s, cached render+text {t2 - t1:.2f} s")
    assert call(PROBE, b"plain words, not a document", room=24)[0] == -2
    assert call(RENDER, made["odt"], 600, 0, room=100)[0] == -4
    print("bad file refused, small buffer refused")
