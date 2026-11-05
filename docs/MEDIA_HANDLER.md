# MEDIA:

`MEDIA:` is a read-only DOS handler on top of `gfopenservice.device`. It
lets shell scripts and programs that know nothing of datatypes decode
files through `media.decode/1`: opening `MEDIA:<kind>/<Amiga path>` reads
the file at that path, has the services card (or a paired Cradle) decode
it, and gives it back in a plain Amiga format.

    Copy MEDIA:ilbm/Work:grad.qoi RAM:grad.iff
    Copy MEDIA:8svx/Work:tone.flac RAM:tone.8svx
    Copy MEDIA:ilbm/Work:test.avi RAM:frame.iff     ; the first frame
    MultiView MEDIA:ilbm/Work:photo.heic

The datatypes keep calling the device directly; the handler is only for
everything else.

## Installing

| File | Where |
| --- | --- |
| `bin/media-handler` | `L:media-handler` |
| `devs/MEDIA` | `DEVS:DOSDrivers/MEDIA` (mounted at boot), or `SYS:Storage/DOSDrivers/MEDIA` and `Mount MEDIA:` |

`gfopenservice.device` must be in `DEVS:`. The handler opens it, and
`media.decode/1`, the first time something asks for a file, and keeps it
open from then on; when there is no card and no paired Cradle it tries
again next time.

## Names

`MEDIA:<kind>[,<width>x<height>]/<path>`

`<path>` is a whole Amiga path, with its own volume or assign
(`Work:grad.qoi`, `DH0:Pics/x.jpg`); a name without one is not found.
The kinds:

| Kind | Pictures | Sounds | Videos |
| --- | --- | --- | --- |
| `ilbm` | 24-bit ILBM | - | the first frame, 24-bit ILBM |
| `ilbm8` | 8-bit ILBM, 256 colours (below) | - | the first frame, 8-bit ILBM |
| `8svx` | - | 8SVX, 8-bit mono, at most 28 kHz | its sound track as 8SVX |
| `aiff` | - | AIFF, 16-bit, the file's channels (at most 2) and rate | its sound track as AIFF |
| `raw` | ARGB pixels | 16-bit PCM | the first frame as ARGB |

`8svx` is for programs and Paula: 8-bit, one channel (stereo is mixed
down), and a rate the Amiga can play (the file's, halved until it is at
most 28 kHz, as `media.decode/1` does). When that loses too much, `aiff`
keeps 16 bits, both channels and the file's own rate.

`,<width>x<height>` (pictures and videos only) is the largest size wanted;
the picture is scaled down to fit, keeping its shape, never up. Without it
pictures come at their own size.

A picture's alpha is dropped (an ILBM has 24 planes, not 32); `raw` keeps
it.

`ilbm8` uses a fixed palette, the 6 x 6 x 6 colour cube that
`media.decode/1`'s 256-colour video frames use (index i < 216 is red
i / 36, green i / 6 % 6, blue i % 6, each step 51, then a grey ramp), with
4 x 4 ordered dithering. Videos' frames come from the card that way;
pictures are dithered on the 68k.

### raw

A 20-byte header of big-endian u32s, then the data:

| Offset | Picture | Sound |
| --- | --- | --- |
| 0 | `'MRAW'` | `'MRAW'` |
| 4 | `'ARGB'` | `'S16B'` |
| 8 | width | rate |
| 12 | height | channels |
| 16 | data bytes | data bytes |

A picture's data is rows of A, R, G, B bytes; a sound's is 16-bit signed
big-endian samples, the channels interleaved.

## Directories

`MEDIA:` holds one directory for each kind; `Dir MEDIA:` lists them. A
kind's directory lists nothing itself, but every directory of the
Amiga's is in it: `MEDIA:ilbm/Work:Pics` is `Work:Pics`, listing the same
entries, so `Dir`, `List`, `CD` and patterns work:

    Copy MEDIA:ilbm/Work:Pics/#?.(qoi|png) RAM:Pics/
    CD MEDIA:ilbm/Work:Pics
    Copy photo.heic RAM:photo.iff

A file's name is its source's own (`grad.qoi`), its date the source's, and
its size the size of the converted file. Examining a file asks the card
for the picture's size or the sound's length (PROBE, or VOPEN for a video)
and works the size out without decoding; ILBMs are written uncompressed,
so their size is known. A directory's listing does not ask the card for
each file: there a file's size is the converted one when it was the last
examined, and 0 (not known) otherwise. Opening a file always converts it.

`NameFromLock()` gives the whole name back (`MEDIA:ilbm/Work:Pics/x.qoi`),
and a source's parents go up through its directories to the kind's.

## How it works

The handler is one process. Opening a file reads the source into memory,
then:

- **Pictures**: PROBE, then DECODE at the size asked, into ARGB; the
  handler writes the ILBM (planes interleaved) and drops the pixels.
- **Videos**: when PROBE does not answer a picture, VOPEN, then VFRAME
  frame 0 (24-bit RGB, or 256 colours for `ilbm8`), then VCLOSE.
- **Sounds**: PROBE only; the samples are decoded as they are read, up to
  1 MB of 16-bit sound at a time, so a long sound never sits in memory
  whole. The source stays in memory while the file is open.

Files open at once each have their own conversion. Seeking works anywhere
in a converted file.

## Errors

| DOS error | When |
| --- | --- |
| 205 `ERROR_OBJECT_NOT_FOUND` | the source does not exist, the kind is unknown, or the path has no volume |
| 236 `ERROR_NOT_IMPLEMENTED` | no services card and no paired Cradle offers `media.decode/1` (or `gfopenservice.device` is not installed) |
| 212 `ERROR_OBJECT_WRONG_TYPE` | the card cannot decode the file, it is not what the kind needs (a sound asked for as `ilbm`, a video without sound as `8svx`), or it is a directory |
| 103 `ERROR_NO_FREE_STORE` | not enough memory for the source or the result |
| 214 `ERROR_DISK_WRITE_PROTECTED` | anything that writes, deletes or renames |
| 210 `ERROR_INVALID_COMPONENT_NAME` | a size that is not `<width>x<height>`, or a source on `MEDIA:` itself |

## Limits

- The source is read whole into memory, as `media.decode/1` takes it in
  one buffer.
- A file straight in a volume's root (`Work:x.qoi`) has the volume as its
  parent, so its `NameFromLock()` is `MEDIA:ilbm/Work:/x.qoi`; files in
  directories are named properly.
- No notification, no links, no writing.
