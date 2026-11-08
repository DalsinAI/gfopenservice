# doc.render/1

Documents for an Amiga's datatypes. The host lays the document out with
LibreOffice or Apache OpenOffice (PostScript with Ghostscript; EPUB and Markdown through pandoc
first), once per file (the PDF is kept in a cache under
`~/.cache/openservice/doc`, or `$OPENSERVICE_CACHE/doc`, named by the
file's hash), and answers with pictures of its pages, or its text. Host
code: `host/doc_render.c`; it runs `soffice`, `gs`, `pandoc`, `pdfinfo`,
`pdftoppm` and `pdftotext`.

Formats, as PROBE names them: `'DOCX'`, `'XLSX'`, `'PPTX'`, `'ODT '`,
`'ODS '`, `'ODP '`, `'ODG '`, `'DOC '`, `'XLS '`, `'PPT '`, `'RTF '`,
`'WPD '` (WordPerfect), `'PDF '`, `'PS  '` and `'EPS '` (an EPS page is
the size of its drawing), `'EPUB'`, and the text formats that have no
signature, named by the extension hint: `'CSV '`, `'TSV '` (comma or tab,
UTF-8), `'MD  '` (also from `'MARK'`), `'HTML'` (also `'HTM '`) and
`'TXT '`.

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | PROBE | buf0 the file; `extra[0]`, `extra[1]` the largest page width and height wanted (0: any); `extra[2]` the extension as a hint, four ASCII letters big-endian, space-padded (0: none); buf1 (out) 24 bytes of info | result the pages |
| 2 | RENDER | `arg` the page (0 the first); buf0 the file; `extra` as PROBE; buf1 (out) the pixels | result width, aux height |
| 3 | TEXT | buf0 the file; `extra[2]` as PROBE; buf1 (out) the text | result its bytes |
| 34 | RELEASE | `arg` a kept file | status -7 when there is no such file |

Files sent once: as media.decode/1's (MEDIA_DECODE.md, "Files sent once"),
`extra[3]` of PROBE, RENDER and TEXT keeps the file on the host (bit 1,
KEEP: its handle to buf3), names a kept one instead of sending it again
(bits 8-31, with an empty buf0) and lets it go after the call (bit 2,
DROP); status -7 says a kept file has gone. gfopendoc.datatype 47.2 sends
a document once: PROBE keeps it, every RENDER names it, the last drops it.

PROBE's info, six big-endian u32s: kind (4 document), format, flags (0),
pages, then the width and height RENDER writes for the same `extra`. A
page's own size is its size at 96 pixels an inch (an A4 page is 794 x
1123); it is scaled down to fit, never up. Every page is drawn at the
first page's size.

RENDER's pixels are rows of 4-byte A, R, G, B (A = 255), as
media.decode/1's. TEXT is ISO-8859-1 with LF line ends, laid out in
columns as on the page (pdftotext `-layout`); when buf1 is too small the
status is -4 and result says the bytes needed.

Status: 0; -2 for a file that is not a document or that the office cannot
open, or a page past the end; -4 for a buf1 too small; -5 when a host tool
failed or no office is installed.

## The office

`$OPENSERVICE_OFFICE` names the office's `soffice` (LibreOffice's or
OpenOffice's); without it, LibreOffice's `soffice` on the `PATH`, else
OpenOffice in `/opt/openoffice4`. OpenOffice is recognised by its
`program/versionrc`; as it has no `--convert-to`, its own Python runs
`host/office_pdf.py` (kept beside `libdocrender.so`), which starts it
headless, opens the document over UNO and stores the PDF. HTML goes
through pandoc first for OpenOffice, whose HTML import loses text. The two
keep separate profiles in the cache. No office: office formats answer -5.

The first request for a file takes about a second (the office starts and
converts it); the ones after it come from the cache in tens of
milliseconds.
