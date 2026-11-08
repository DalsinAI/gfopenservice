#!/bin/sh
# NDECODE's decoders (host/media_native.c) for this computer: gfopendatatypes'
# own picture decoders (png/pngdecode.c, tga/tgadecode.c, gif/gifdecode.c)
# and libwebp 1.6.0 with webp.datatype's three patches, the same sources the
# 68k datatypes are built from, into host/obj-native/libnative.a. libwebp's
# plain C paths only (no SSE2 or SSE4.1 files), as on the 68k; every symbol
# hidden, so FFmpeg's own libwebp is never mixed up with ours.
#
# Prints the compiler flags media_native.c needs, or fails (nothing printed)
# when the checkout or the tarball is not to hand.
#
#   $1        the gfopendatatypes checkout (default: beside this one)
#   TARBALLS  folder holding libwebp-1.6.0.tar.gz (default: the checkout's
#             tarballs/); its SHA-256 is checked
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ODT=${1:-"$HERE/../../gfopendatatypes"}
D="$ODT/Datatypes"
TARBALLS=${TARBALLS:-"$ODT/tarballs"}
T="$TARBALLS/libwebp-1.6.0.tar.gz"
SUM=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
[ -f "$D/png/pngdecode.c" ] && [ -f "$D/tests/host/exec/types.h" ] || exit 1
if [ ! -f "$T" ] && [ -f "$ODT/tools/fetch-tarball.sh" ]; then
    sh "$ODT/tools/fetch-tarball.sh" libwebp-1.6.0.tar.gz "$TARBALLS" >/dev/null 2>&1 || exit 1
fi
[ -f "$T" ] || exit 1
echo "$SUM  $T" | sha256sum -c - >/dev/null 2>&1 || { echo "build_native: SHA-256 mismatch: $T" >&2; exit 1; }
OBJ="$HERE/obj-native"
rm -rf "$OBJ"; mkdir -p "$OBJ/src/webp"
tar xf "$T" -C "$OBJ"
W="$OBJ/libwebp-1.6.0"
for p in no-float demux-null-chunk encoder-no-float; do
    (cd "$W" && patch -p1 -s < "$D/patches/libwebp-1.6.0-$p.patch")
done
# HAVE_CONFIG_H with an empty config.h: libwebp then uses no SSE2 or SSE4.1
# code even though x86-64 compilers define __SSE2__.
: > "$OBJ/src/webp/config.h"
CC=${CC:-cc}
F="-O2 -fPIC -fvisibility=hidden -DWEBP_EXTERN=extern -DHAVE_CONFIG_H -I$OBJ -I$W -I$W/src -I$D/tests/host"
set -- dec/alpha_dec dec/buffer_dec dec/frame_dec dec/idec_dec dec/io_dec dec/quant_dec dec/tree_dec dec/vp8_dec \
    dec/vp8l_dec dec/webp_dec demux/demux dsp/alpha_processing dsp/cpu dsp/dec dsp/dec_clip_tables dsp/filters \
    dsp/lossless dsp/rescaler dsp/upsampling dsp/yuv utils/bit_reader_utils utils/color_cache_utils \
    utils/filters_utils utils/huffman_utils utils/palette utils/quant_levels_dec_utils utils/random_utils \
    utils/rescaler_utils utils/thread_utils utils/utils
objs=
for f in "$@"; do
    o="$OBJ/$(echo "$f" | tr '/' '_').o"
    # shellcheck disable=SC2086
    $CC $F -c "$W/src/$f.c" -o "$o"
    objs="$objs $o"
done
for f in png/pngdecode tga/tgadecode gif/gifdecode; do
    o="$OBJ/$(basename "$f").o"
    # shellcheck disable=SC2086
    $CC $F -Wall -c "$D/$f.c" -o "$o"
    objs="$objs $o"
done
rm -f "$OBJ/libnative.a"
# shellcheck disable=SC2086
ar rcs "$OBJ/libnative.a" $objs
echo "-DMD_NATIVE -DWEBP_EXTERN=extern -I$D/tests/host -I$D/png -I$D/tga -I$D/gif -I$W/src"
