# Contributors

## Creator and maintainer

- **SacredTrees** ([@SacredTrees](https://github.com/SacredTrees)): created Nursery, designs it and maintains it.

## The AmigaChrome team

We are The AmigaChrome Team, who build Nursery alongside SacredTrees:

- **Agnus**, our coordinator, who keeps every thread moving.
- **Thufir**, **Kynes** and **Galen**, who started the work on SacredTrees's x86 cores.
- **The threads**, each one taking a piece of the work from design to release.

## Copyright holder

Nursery's own code, `gfopenservice.device`, the Cradle-side services and the documents are
Copyright (c) 2026 AmigaChrome Team, released under the MIT licence (`LICENSE`).

## Third-party work in this repository

Two parts of the LAN link's cryptography in `src/oscrypto.c` are adapted
from public-domain code, as the comments there say.

| Component | Where | Authors | Licence |
| --- | --- | --- | --- |
| TweetNaCl's `crypto_scalarmult` (X25519) | `src/oscrypto.c` | Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe and Sjaak Smetsers | Public domain |
| poly1305-donna (Poly1305 with 26-bit limbs) | `src/oscrypto.c` | Andrew Moon | Public domain |

## Used at build or run time, not included

The Cradle-side services hand work to libraries and tools installed on the
host, each under its own licence:

- **OpenSSL 3** (`opentls.key/1`, and the `opentls` provider through AmiSSL 5 on the Amiga).
- **libavif**, **libheif** with libde265, **FFmpeg**, **ImageMagick**, **LibRaw**, **librsvg** and **cairo**, **FluidSynth** and **sidplayfp** (`media.decode/1`, `media.cdxl/1`).
- **LibreOffice** or **Apache OpenOffice**, **poppler**, **Ghostscript** and **pandoc** (`doc.render/1`).
- **zlib**, linked from the host system.

Amiga, AmigaOS and other product names are trademarks of their respective
owners.
