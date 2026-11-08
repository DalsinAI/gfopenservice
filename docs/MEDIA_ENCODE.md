# media.encode/1

Writes pictures, sounds and films for an Amiga's datatypes in the formats
the Team ruled (6 November 2026) are written by the service's FFmpeg and
never by code in the datatype pack: HEIC pictures, AAC (M4A or ADTS) and WMA
sounds, HEVC and WMV films. The Amiga sends the object's pixels, samples or
frames; the host writes them to files in a private folder, runs the `ffmpeg`
program on them, and hands the file back in pieces.

Host code: `host/media_encode.c` (entry `me_call`, the same arguments as
`md_call`), built as `host/libmediaencode.so`. It never links FFmpeg: the
encoders (x265, FFmpeg's AAC, WMA 2 and WMV 8) run in the `ffmpeg` process,
so their licences and patents stay with that program on the host, and
nothing patent-encumbered or GPL reaches the Amiga or this library. HEIC's
container (HEIF boxes around FFmpeg's HEVC) is our own code,
`host/heif_mux.c`, from ISO/IEC 23008-12 and 14496-12/15.

`OPENSERVICE_FFMPEG` names the program (default `ffmpeg` on `PATH`).

The Amiga side is gfopendatatypes' `gfopenpicture.datatype` (HEIC),
`gfopensound.datatype` (M4A, AAC, WMA) and `gfopenvideo.datatype` (HEVC,
WMV), answering `DTM_WRITE` in raw mode (`Datatypes/common/dtencode.c`).

## Operations

The numbers start at 16, clear of `media.decode/1`'s, so the two never
meet even where one table serves both.

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 16 | FORMATS | buf1 (out) room for the list | buf1 the formats it writes, big-endian u32s; result how many (status -4 when buf1 holds fewer) |
| 17 | BEGIN | buf0 the settings (48 bytes, below) | result the job |
| 18 | PUT | `arg` the job; `extra[0]` the stream (0 pixels, 1 sound); buf0 the next bytes | result that stream's bytes so far |
| 19 | FINISH | `arg` the job; buf1 (out, optional) 24 bytes of info | result the file's bytes; aux 1 (result 0) while FFmpeg is still at work |
| 20 | READ | `arg` the job; `extra[0]` the offset; buf1 (out) room for a piece | result the bytes written (0 at the end) |
| 21 | CLOSE | `arg` the job | |

FORMATS is the capability check: the list depends on what the host's
`ffmpeg` has (`ffmpeg -encoders` and `-muxers`, asked once): `'HEIC'` needs
libx265, `'M4A '` AAC and the ipod muxer, `'AAC '` AAC and ADTS, `'WMA '`
WMA 2 and ASF, `'HEVC'` libx265, AAC and MP4, `'WMV '` WMV 8, WMA 2 and ASF.
With no `ffmpeg` the list is empty and BEGIN refuses everything.

FINISH starts FFmpeg on a thread of its own and waits for it at most 20
seconds (`OPENSERVICE_ENCODE_WAIT`, in ms, for tests): a long film answers
aux 1 and the Amiga sends FINISH again, so no request goes near a LAN
request's 60 seconds and no emulator worker is held for minutes. A CLOSE
while FFmpeg works lets the job go when it is done.

### Settings

Twelve big-endian u32s:

| Offset | Field |
| --- | --- |
| 0 | version, 1 |
| 4 | the file to write: `'HEIC'`, `'M4A '`, `'AAC '`, `'WMA '`, `'HEVC'`, `'WMV '` |
| 8, 12 | width, height (pictures and films) |
| 16 | pixels: 0 ARGB (4 bytes, picture.datatype's `PBPAFMT_ARGB`), 1 RGB (3 bytes) |
| 20 | films: frames a second x 1000 |
| 24 | samples a second; 0 for a film without sound |
| 28 | channels, 1 or 2 |
| 32 | samples: 0 16-bit signed big-endian, 1 8-bit signed |
| 36 | quality, 1 (smallest) to 100 (best); 0 the format's default |
| 40 | flags: bit 0 keep the alpha (HEIC) |
| 44 | 0 |

Stream 0 is the pixels, rows packed, a film's frames one after another;
stream 1 the samples, channels interleaved. PUT them in pieces of any size,
cut anywhere. FINISH refuses (-2) a picture short of its bytes, a film that
is not whole frames, and sound that is not whole sample frames.

FINISH's info, six big-endian u32s: the format, the file's bytes, width,
height, frames, sample frames.

Status: 0; -2 for bad settings, a format not offered, a job that is not
there (closed, or the oldest of nine sent away) or busy, the wrong stream,
too many bytes; -4 when buf1 is too small; -5 when FFmpeg fails or is
missing.

## What is written

| Format | File | Codec | Default | Quality 1 to 100 |
| --- | --- | --- | --- | --- |
| `'HEIC'` | HEIF (`.heic`, brands heic, mif1) | HEVC Main still (x265), 4:2:0, full-range BT.601 YCbCr; alpha as a second, monochrome HEVC item (`auxl`, `urn:mpeg:hevc:2015:auxid:1`) | CRF 20 | CRF 51 to 0 |
| `'M4A '` | MP4 audio (`.m4a`), fast start | AAC-LC | 64 kbit/s a channel | 16 to 160 kbit/s a channel |
| `'AAC '` | ADTS (`.aac`) | AAC-LC | as M4A | as M4A |
| `'WMA '` | ASF (`.wma`) | WMA 2 | as M4A, at most about 3 bits a sample | as M4A |
| `'HEVC'` | MP4 (`.mp4`), `hvc1`, fast start | HEVC (x265, preset fast), 4:2:0; AAC-LC sound | CRF 23 | CRF 51 to 0 |
| `'WMV '` | ASF (`.wmv`) | WMV 8; WMA 2 sound | qscale 5 | qscale 31 to 2 |

Sound is written at the usual rate at or above its own (8000, 11025, 16000,
22050, 32000, 44100 or 48000 Hz): an Amiga's 28000 Hz becomes 32000 Hz.

A picture or film with an odd side is coded one pixel bigger, the last row
or column repeated (4:2:0 needs even sides). A HEIC says the real size with
a `clap` box, so readers show it exactly; a film keeps the extra pixel.
Pictures are at most 8192 a side (one HEVC picture, no grid), films 4096.

A HEIC's colour is said twice, in the HEVC's VUI and in an `nclx` `colr`
box: sRGB primaries and transfer, BT.601 matrix, full range (as JPEG).

## Where it runs

- **The LAN Cradle**: `cradle/nursery_host.py` offers `media.encode/1` when
  `host/libmediaencode.so` is built.
- **The emulator's services card** opens host libraries by name from its
  table (amicradle `machines/motherboards/a1200-x86/native/services.c`,
  `host_libs[]`): `media.encode/1` needs its row there,
  `{ "media.encode/1", "libmediaencode.so", "me_call", NULL, NULL }`, and its
  handler beside the others. Until then the emulator's machines have no
  `media.encode/1` and the datatypes say so (`ERROR_NOT_IMPLEMENTED`).

## Tests

`host/test_media_encode.py` (after `./build.sh`, or the one `gcc` line for
`libmediaencode.so`): every format against outside readers. HEIC files are
read by libheif (size after `clap`, alpha; their pixels too where libheif
has its HEVC decoder plugin) and their items' HEVC decoded by FFmpeg and
compared with the pixels sent, alpha included; sounds and films are read
back by ffprobe and ffmpeg (container, codec, rate, channels, size, frame
count, a 440 Hz tone, PSNR of a frame); then the requests it must refuse,
FINISH's "still working" answers, a CLOSE while FFmpeg works, and no
`ffmpeg` at all. On an x86-64 Linux host (FFmpeg 6.1.1, libheif 1.17.6), a
320x240 HEIC: about 0.3 s, 49 dB (libheif's decode and FFmpeg's agree);
two seconds of stereo sound: 0.2 to 0.5 s; fifty 160x120 frames: 0.3 to
0.7 s.
