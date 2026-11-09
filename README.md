# gfopenservice: Nursery

**Nursery** is the whole system: `gfopenservice.device` on the Amiga, the
AutoConfig boards that carry services (the services card, CPU cores), the
paired Cradles on the LAN, and the services they run. This repository holds
its Amiga side, the `Nursery` command that lists what is there, and the
Cradle-side services.

`gfopenservice.device` hands work from an Amiga to a named service, such as
`opentls.key/1` for TLS key maths, `media.decode/1` and `media.encode/1` for
datatypes, and `openweb.fetch/1` for HTTP and HTTPS.
Programs ask by name and do not know where the work runs:

- **On a board in this machine.** Every AmiCradle Team AutoConfig board with the
  services block: the services card, CPU cores, FPU or TPU boards. On
  AmiCradle (on x86 cores, or the appliance) these are virtual and the work runs on
  the x86 or ARM64 cores.
- **On a Cradle on the LAN.** A real Amiga with no such board asks over mDNS
  which Cradle offers the service, and uses it once paired.
- **Nowhere.** OPEN fails, and the program does the work itself on the 68k.

## What is here

| Path | What |
| --- | --- |
| `include/devices/gfopenservice.h` | The device's API |
| `src/device.c` | `gfopenservice.device`: boards' rings, LAN connections, one worker process |
| `src/mdns.c` | One mDNS question for `_amicradle._tcp.local` |
| `src/nursery.c` | `Nursery`: lists the boards and the Cradles on the LAN, and their services |
| `library/mitchy_lib.c`, `include/libraries/mitchy.h` | `mitchy.library`: the services for any program, by name, with no device I/O to write: open, requests waited for or sent (a signal or a message on completion), cancel, the boards (Mitchy, AkikoPlus, the CPU cores board), and a plain "not fitted" answer. On `gfopenservice.device`. Its autodoc is `docs/mitchy.doc`; `sfd/mitchy_lib.sfd` makes `fd/mitchy_lib.fd` and the `clib`, `proto`, `inline` and `pragmas` headers (`library/gen_headers.sh`) |
| `src/servicetest.c` | `ServiceTest` 0.2, on `mitchy.library`: the boards, the list, `echo/1` waited for, in parallel by signal and by message, cancelled, too small and badly flagged, and one checked job for each real service on offer; returns 5 when no card or Cradle offers the services |
| `cradle/` | A Cradle's side for the LAN: mDNS advertising, and a host for `echo/1`, `opentls.key/1`, `media.decode/1`, `media.encode/1` and `doc.render/1` |
| `host/opentls_key.c` | `opentls.key/1` on the host (docs/OPENTLS_KEY.md) |
| `host/doc_render.c` | `doc.render/1` on the host: office documents as pages and text, laid out by LibreOffice or Apache OpenOffice (docs/DOC_RENDER.md); `host/office_pdf.py` drives OpenOffice |
| `host/cdxl.c`, `host/media_cdxl.py` | `media.cdxl/1`: any movie into CDXL for an ECS, AGA or RTG Amiga, as an option (docs/MEDIA_CDXL.md); `media_cdxl.py` does it from the Cradle's command line |
| `src/tocdxl.c` | `ToCDXL`, the Amiga command for `media.cdxl/1` |
| `src/mediahandler.c`, `devs/MEDIA` | `MEDIA:`, a read-only DOS handler (`L:media-handler`) that decodes files through `media.decode/1` for shell scripts and programs without datatypes: `Copy MEDIA:ilbm/Work:grad.qoi RAM:grad.iff` (docs/MEDIA_HANDLER.md) |
| `src/webfetch.c`, `include/openweb/webfetch.h` | The Amiga's side of `openweb.fetch/1`, for any program to link (docs/OPENWEB_FETCH.md) |
| `src/wfnet*.c`, `include/openweb/wfnet.h` | The native transport of `openweb.fetch`: HTTP/1.1, HTTP/2 (our own HPACK), WebSocket, proxies and HTTP auth over the Amiga's bsdsocket.library and gfopentls.library, when no card or Cradle offers it; gzip, deflate and Brotli bodies undone by the datatypes (`src/wfnet_dec.c`: stream.datatype's gzip.datatype and brotli.datatype through datatypes.library); portable C, tested on Linux through `host/wfnet_posix.c` |
| `src/owfetch.c` | `OWFetch`: fetches a URL through `openweb.fetch/1`, prints the status and headers, writes the body |
| `host/openweb_fetch.c` | `openweb.fetch/1` on the host: HTTP and HTTPS fetches with OpenSSL 3, zlib and Brotli, many at once, each with its own handle (docs/OPENWEB_FETCH.md) |
| `host/hostrun.c` | Running host tools (LibreOffice, poppler, ImageMagick, LibRaw, FluidSynth, sidplayfp) and the services' result cache |
| `host/media_decode.c`, `host/media_av.c`, `host/media_tool.c`, `host/media_svg.c` | `media.decode/1` on the host: pictures (AVIF, HEIC, JPEG XL, camera RAW, PSD, SVG drawn at any size and anything FFmpeg or ImageMagick reads), sounds, MIDI, SID and video, for gfopenimage's datatypes (docs/MEDIA_DECODE.md); WebM frames by packet in webm.datatype's own colours |
| `host/media_native.c`, `host/build_native.sh` | `media.decode/1`'s NDECODE: the Amiga's own PNG, TGA, GIF and WebP decoders, built for the host from the gfopendatatypes checkout, so the services card gives the 68k's pixels byte for byte |
| `host/blob.c` | Files sent once: kept on the host by handle for `media.decode/1` and `doc.render/1` |
| `host/media_encode.c`, `host/heif_mux.c` | `media.encode/1` on the host: HEIC pictures, AAC/M4A and WMA sounds, HEVC and WMV films written by the `ffmpeg` program in its own process from the pixels, samples and frames an Amiga's datatypes send; HEIC's container is our own (docs/MEDIA_ENCODE.md) |
| `opentls/` | An OpenSSL 3 provider for AmiSSL that sends a TLS handshake's key maths to `opentls.key/1`, and does it on the 68k when nothing answers |

The board's registers and rings, and the LAN framing, are in the AmiCradle
project's `design/SERVICES_CARD.md` (v1); the emulator side is built there.

## mitchy.library

```c
struct Library *MitchyBase = OpenLibrary("mitchy.library", 1);
LONG err;
struct MitchyService *svc = MitchyOpen("media.decode/1", 0, &err);
if (!svc) { /* err MERR_NOTFITTED: no services card here, and no paired Cradle: do it on the 68k */ }
struct MitchyRequest *req = MitchyAllocRequest(svc, NULL);   /* its own signal: req->mr_SigMask */
req->mr_Op = 1;
req->mr_Flags = MBF_BUF1;                                    /* buffer 1 is written by the service */
req->mr_Buf[0].mb_Data = file;  req->mr_Buf[0].mb_Length = fileLength;
req->mr_Buf[1].mb_Data = info;  req->mr_Buf[1].mb_Length = 24;
MitchyDoRequest(req);                                        /* or MitchySendRequest, Wait(mr_SigMask), MitchyCheckRequest */
/* req->mr_Status, mr_Result, mr_Aux */
MitchyFreeRequest(req);
MitchyClose(svc);
```

Requests made with a port instead complete as messages on it
(`MitchyRequestFromMsg`); `MitchyAbortRequest` cancels one at once;
`MitchyCall` is one call from any task; `MitchyFindBoard` finds the ACSV
boards by CLASS (AkikoPlus's `akikoplus.library` opens its services through
this library and reaches its bank windows from its ConfigDev). The library
never touches a board's rings: `gfopenservice.device` owns them, so every
program shares them. Install it in `LIBS:` beside `DEVS:gfopenservice.device`.

## opentls

A program using AmiSSL calls `opentls_amiga_open()` after opening AmiSSL.
When a board or a paired Cradle offers `opentls.key/1`, every handshake's
certificate checks, the server's handshake signature and the key exchange
go there; TLS itself, and the bulk encryption, stay on the Amiga.
`opentls/tlstest.c` runs the same provider on Linux against real sites:
every handshake to example.com, login.live.com (TLS 1.2), github.com,
wikipedia.org, google.com and others completes with all key work sent;
bad certificates still fail, a service that says "bad" fails every
handshake, and with no service the work is done locally.
`opentls/tlsprobe.c` times handshakes on the Amiga through curl.

## Pairing

`Nursery PAIR=<Cradle>` pairs this Amiga with a Cradle: both show a
six-digit code, and once they match the Cradle's services are sealed with
the key they agreed (ChaCha20-Poly1305). `docs/PAIRING.md` has the details.
A paired service is opened on every paired Cradle that offers it, and each
call goes to the least busy one, so a cluster of Cradles shares the work.

## Building

`./build.sh`, with the os32-gcc16 compiler (`OS32_GCC16`), and `OWFetch`
with the os32 stove's GCC 6.5 (`OS32`), with the gfopentls and
gfopendatatypes checkouts beside this one (`OPENTLS`, `ODT`) for their
headers. Install `gfopenservice.device` in `DEVS:`, `mitchy.library` in `LIBS:`, and for `MEDIA:`
`bin/media-handler` as `L:media-handler` and `devs/MEDIA` in
`DEVS:DOSDrivers`. The native transport
undoes compressed bodies when stream.datatype, gzip.datatype and
brotli.datatype are in `SYS:Classes/DataTypes` (and z.library in `LIBS:`
for gzip); without them it asks for them uncoded.

The host side needs, on the Cradle: OpenSSL 3 for `opentls.key/1`, OpenSSL
3's TLS and zlib for `openweb.fetch/1`, with libbrotlidec for its Brotli
(on Ubuntu: `libssl-dev zlib1g-dev libbrotli-dev`), and
libavif and libheif with libheif's HEVC decoder plugin for `media.decode/1`
and FFmpeg's libraries for its sounds (on Ubuntu: `libavif-dev libheif-dev
libheif-plugin-libde265 libavformat-dev libavcodec-dev libswresample-dev`).
Without the HEVC plugin HEIC files are refused; without FFmpeg, sounds are.
NDECODE is built in when the gfopendatatypes checkout (`ODT`) and its
`tarballs/libwebp-1.6.0.tar.gz` are there (`host/build_native.sh`).
The rest of `media.decode/1`'s formats need tools at run time only:
`imagemagick` for PSD, XCF, TGA and other pictures, `libraw-bin` for camera
RAW, `fluidsynth` with `fluid-soundfont-gm` for MIDI, `sidplayfp` for
SID tunes, and `librsvg2-2` (on most desktops already) for SVG; a missing tool just means that format is refused.
`doc.render/1` runs LibreOffice (`libreoffice-writer`, `-calc`,
`-impress`), or Apache OpenOffice 4 (with its `pyuno` part) when that is
what is installed, and poppler's tools (`poppler-utils`), with `ghostscript` for
PostScript and `pandoc` for EPUB and Markdown. `host/test_*.py` check
each service with files made by `avifenc`, `heif-enc`, `ffmpeg` and
LibreOffice; `host/test_openweb_fetch.py` against HTTP and HTTPS servers it
starts on 127.0.0.1, with a throwaway certificate it makes with `openssl`;
`host/test_wfnet.py` the native transport against HTTP/1.1, HTTP/2,
WebSocket and proxy servers it starts, and RFC 7541's examples, and its
decoders over gfopendatatypes' stream core when that checkout is there.

Measured on AmigaOS 3.2.3 (AmiCradle, GFOpenSocket) against a Cradle on
other x86 cores: 500 `echo/1` calls, all correct, 2.2 ms each.

## Licence

MIT, Copyright (c) 2026 AmiCradle Team. See LICENSE.

## Contributors

Nursery is created and maintained by [SacredTrees](https://github.com/SacredTrees) with The AmiCradle Team, copyright AmiCradle Team. Everyone whose work it includes is credited in [`CONTRIBUTORS.md`](CONTRIBUTORS.md).
