#!/bin/sh
# For the Cradle's stove_build (STOVE= and OUT= set): the two devices, Nursery and ServiceTest.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
CC=$STOVE/prefix/bin/m68k-amigaos-gcc
SOFT="-m68040 -specs=$HERE/build/m68k-soft-m68040.specs"
mkdir -p "$OUT" obj
for f in start device mdns paired oscrypto osrandom alias; do
    "$CC" $SOFT -Os -fno-delete-null-pointer-checks -Wall -fomit-frame-pointer -Iinclude -c src/$f.c -o obj/$f.o
done
"$CC" -nostartfiles $SOFT -o "$OUT/mitchy.device" obj/start.o obj/device.o obj/mdns.o obj/paired.o obj/oscrypto.o obj/osrandom.o -lamiga
"$CC" -nostartfiles $SOFT -o "$OUT/gfopenservice.device" obj/start.o obj/alias.o -lamiga
"$CC" $SOFT -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o "$OUT/Nursery" src/nursery.c src/mdns.c src/paired.c src/oscrypto.c src/osrandom.c -lamiga
"$CC" $SOFT -mcrt=nix20 -Os -fno-delete-null-pointer-checks -Wall -Iinclude -o "$OUT/ServiceTest" src/servicetest.c -lamiga
