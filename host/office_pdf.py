"""doc.render/1's PDF step through Apache OpenOffice, which has no --convert-to:
starts its soffice headless, opens the document over UNO and stores a PDF.
Run by OpenOffice's own Python (program/python, which has its uno module):

  python office_pdf.py SOFFICE PROFILE-URL SOURCE PDF [CSV-OPTIONS]

Python 2.7 and 3. MIT, Copyright (c) 2026 AmiCradle Team."""
import os
import subprocess
import sys
import time

import uno
from com.sun.star.beans import PropertyValue

FILTERS = (
    ("com.sun.star.text.WebDocument", "writer_web_pdf_Export"),
    ("com.sun.star.text.TextDocument", "writer_pdf_Export"),
    ("com.sun.star.sheet.SpreadsheetDocument", "calc_pdf_Export"),
    ("com.sun.star.presentation.PresentationDocument", "impress_pdf_Export"),
    ("com.sun.star.drawing.DrawingDocument", "draw_pdf_Export"),
)


def prop(name, value):
    p = PropertyValue()
    p.Name = name
    p.Value = value
    return p


def url(path):
    return uno.systemPathToFileUrl(os.path.abspath(path))


def main(soffice, profile, src, pdf, csv=None):
    pipe = "openservice%d" % os.getpid()
    office = subprocess.Popen([soffice, "-env:UserInstallation=" + profile, "-headless", "-invisible",
                               "-norestore", "-nolockcheck", "-nodefault", "-nofirststartwizard",
                               "-accept=pipe,name=%s;urp;" % pipe])
    desktop = None
    try:
        local = uno.getComponentContext()
        resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
        ctx = None
        for _ in range(240):                     # up to a minute for a cold start
            try:
                ctx = resolver.resolve("uno:pipe,name=%s;urp;StarOffice.ComponentContext" % pipe)
                break
            except Exception:
                if office.poll() is not None:
                    return 1
                time.sleep(0.25)
        if ctx is None:
            return 1
        desktop = ctx.ServiceManager.createInstanceWithContext("com.sun.star.frame.Desktop", ctx)
        load = [prop("Hidden", True), prop("ReadOnly", True)]
        if csv:
            load += [prop("FilterName", "Text - txt - csv (StarCalc)"), prop("FilterOptions", csv)]
        doc = desktop.loadComponentFromURL(url(src), "_blank", 0, tuple(load))
        if doc is None:
            return 1
        try:
            name = next((f for kind, f in FILTERS if doc.supportsService(kind)), "writer_pdf_Export")
            doc.storeToURL(url(pdf), (prop("FilterName", name),))
        finally:
            doc.close(True)
        return 0
    finally:
        if desktop is not None:
            try:
                desktop.terminate()
            except Exception:
                pass                             # the bridge goes as soffice quits
        try:
            office.wait(timeout=10) if sys.version_info[0] >= 3 else None
        except Exception:
            pass
        if office.poll() is None:
            for _ in range(40):
                if office.poll() is not None:
                    break
                time.sleep(0.25)
            else:
                office.kill()


if __name__ == "__main__":
    if len(sys.argv) not in (5, 6):
        sys.stderr.write(__doc__)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
