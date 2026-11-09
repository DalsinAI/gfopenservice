#!/bin/sh
# Builds gfopenservice.device, mitchy.library, Nursery, ServiceTest, ToCDXL and media-handler for AmigaOS 3.x with the
# os32-gcc16 compiler (GCC 16.2, libnix), OWFetch with the os32 stove (GCC 6.5),
# and the host side's libraries: TLS keys, media decoding and encoding, documents and web fetches.
#   OS32_GCC16  compiler root holding prefix/ (default ~/AmiCradle/stoves/os32-gcc16)
# MIT, Copyright (c) 2026 AmiCradle Team.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
P=${OS32_GCC16:-"$HOME/AmiCradle/stoves/os32-gcc16"}/prefix
CC="$P/bin/m68k-amigaos-gcc"
mkdir -p "$HERE/bin" "$HERE/obj"
# The 68040, soft float (one binary for full and LC/EC 68040s): the specs
# file links libnix's soft libm (maths through the mathieee libraries),
# not its FPU libm881.
SOFT="-m68040 -specs=$HERE/build/m68k-soft-m68040.specs"
cd "$HERE"
# The device: its own ROMTag, no C library start-up; start.c is linked first.
for f in start device mdns paired oscrypto osrandom; do
    "$CC" $SOFT -Os -fno-delete-null-pointer-checks -Wall -fomit-frame-pointer -Iinclude -c src/$f.c -o obj/$f.o
done
"$CC" -nostartfiles $SOFT -o bin/gfopenservice.device obj/start.o obj/device.o obj/mdns.o obj/paired.o obj/oscrypto.o obj/osrandom.o \
    -lamiga -Wl,-Map=obj/gfopenservice.device.map
# mitchy.library (library/, include/libraries/mitchy.h): bare, no C library
# or start-up code; mitchy_lib.c's start() and RomTag lead the hunk. Its own
# memcpy and memset: -fno-tree-loop-distribute-patterns keeps GCC from
# turning their loops into calls to themselves. Its autodoc, docs/mitchy.doc.
"$CC" $SOFT -Os -fno-delete-null-pointer-checks -Wall -Wextra -Werror -Wno-unused-parameter -fomit-frame-pointer \
    -fno-toplevel-reorder -fno-builtin -fno-tree-loop-distribute-patterns -nostartfiles -nostdlib -Iinclude \
    -o bin/mitchy.library library/mitchy_lib.c -lgcc
python3 library/autodoc.py library/mitchy_lib.c > docs/mitchy.doc
"$CC" $SOFT -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o bin/Nursery src/nursery.c src/mdns.c src/paired.c src/oscrypto.c src/osrandom.c -lamiga
"$CC" $SOFT -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o bin/ServiceTest src/servicetest.c -lamiga
"$CC" $SOFT -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o bin/ToCDXL src/tocdxl.c -lamiga
# MEDIA:'s handler: started by DOS as a C handler (GlobVec -1), no C library
# start-up; mhstart.c is linked first. Its mountlist is devs/MEDIA.
"$CC" -nostartfiles $SOFT -Os -fno-delete-null-pointer-checks -Wall -fomit-frame-pointer -Iinclude -o bin/media-handler \
    src/mhstart.c src/mediahandler.c -lamiga
# OWFetch and its client (src/webfetch.c, for other programs to link), with
# the os32 stove's GCC 6.5 and libnix.
#   OS32  the stove holding prefix/ (default ~/AmiCradle/stoves/os32)
CC65=${OS32:-"$HOME/AmiCradle/stoves/os32"}/prefix/bin/m68k-amigaos-gcc
# The native transport (src/wfnet*.c: HTTP/1.1, HTTP/2, WebSocket, proxies,
# auth) comes with it, over bsdsocket.library and gfopentls.library, whose
# headers are GFOpenTLS's, and datatypes.library for compressed bodies, with
# stream.datatype's header from gfopendatatypes.
#   OPENTLS  the gfopentls checkout (default: beside this one)
#   ODT      the gfopendatatypes checkout (default: beside this one)
OPENTLS=${OPENTLS:-"$HERE/../gfopentls"}
ODT=${ODT:-"$HERE/../gfopendatatypes"}
WFNET="src/wfnet.c src/wfnet_h2.c src/wfnet_hpack.c src/wfnet_auth.c src/wfnet_dec.c src/wfnet_amiga.c"
"$CC65" $SOFT -noixemul -Os -fno-delete-null-pointer-checks -Wall -Werror -Iinclude -Isrc -I"$OPENTLS/include" \
    -I"$ODT/Datatypes/include" -o bin/OWFetch src/owfetch.c src/webfetch.c $WFNET
# The host side (Linux, OpenSSL 3), for the LAN Cradle and the emulator.
if [ -f /usr/include/openssl/evp.h ]; then
    gcc -O2 -Wall -fPIC -shared -o host/libopentlskey.so host/opentls_key.c -lcrypto
fi
# media.decode/1: pictures with libavif, and libheif with its HEVC decoder
# plugin; sounds, video and most pictures with FFmpeg when its headers are
# there; RAW, ImageMagick, MIDI and SID through those tools at run time.
# NDECODE (the Amiga's own picture decoders, docs/MEDIA_DECODE.md) when the
# gfopendatatypes checkout and libwebp's tarball are to hand
# (host/build_native.sh); without them NDECODE says no and the Amiga decodes.
NATIVE_CFLAGS= NATIVE_LIB=
if NATIVE_CFLAGS=$(sh host/build_native.sh "$ODT"); then
    NATIVE_LIB=host/obj-native/libnative.a
else
    NATIVE_CFLAGS=
fi
MD_SRC="host/media_decode.c host/media_native.c host/blob.c host/media_tool.c host/media_svg.c host/hostrun.c"
if pkg-config --exists libavif libheif 2>/dev/null; then
    if pkg-config --exists libavformat libavcodec libswresample libswscale libavutil 2>/dev/null; then
        gcc -O2 -Wall -DMD_AV $NATIVE_CFLAGS -fPIC -shared -Wl,-Bsymbolic -o host/libmediadecode.so $MD_SRC host/media_av.c host/cdxl.c host/ham8.c \
            $NATIVE_LIB -lpthread -lz -ldl -lm \
            $(pkg-config --cflags --libs libavif libheif libavformat libavcodec libswresample libswscale libavutil)
    else
        gcc -O2 -Wall $NATIVE_CFLAGS -fPIC -shared -Wl,-Bsymbolic -o host/libmediadecode.so $MD_SRC $NATIVE_LIB -lpthread -lz -ldl -lm \
            $(pkg-config --cflags --libs libavif libheif)
    fi
fi
# media.encode/1: runs the ffmpeg program at run time (its own process: no
# FFmpeg headers or libraries here); HEIC's container is our own code.
gcc -O2 -Wall -fPIC -shared -o host/libmediaencode.so host/media_encode.c host/heif_mux.c host/hostrun.c -lpthread
# doc.render/1: needs LibreOffice and poppler's tools at run time only.
gcc -O2 -Wall -fPIC -shared -o host/libdocrender.so host/doc_render.c host/blob.c host/hostrun.c -lpthread -ldl
# The native transport's own code on Linux, for host/test_wfnet.py: POSIX
# sockets, and OpenSSL and zlib standing in for GFOpenTLS and the datatypes.
if [ -f /usr/include/openssl/ssl.h ] && [ -f /usr/include/zlib.h ]; then
    gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared -Iinclude -Isrc -o host/libwfnet.so host/wfnet_posix.c \
        src/wfnet.c src/wfnet_h2.c src/wfnet_hpack.c src/wfnet_auth.c -lssl -lcrypto -lz
fi
# openweb.fetch/1: OpenSSL 3's TLS and zlib; Brotli too when its decoder's headers are there.
if [ -f /usr/include/openssl/ssl.h ] && [ -f /usr/include/zlib.h ]; then
    if [ -f /usr/include/brotli/decode.h ]; then
        gcc -O2 -Wall -DWF_BROTLI -fPIC -shared -o host/libopenwebfetch.so host/openweb_fetch.c -lssl -lcrypto -lz -lbrotlidec -lpthread
    else
        gcc -O2 -Wall -fPIC -shared -o host/libopenwebfetch.so host/openweb_fetch.c -lssl -lcrypto -lz -lpthread
    fi
fi
ls -l bin
