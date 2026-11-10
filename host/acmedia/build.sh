#!/bin/sh
# Builds ACMedia's own FFmpeg (design section 4) for this machine, static, LGPL
# only, from the pinned sources (PINS, fetch.sh): dav1d, libopenmpt, then
# FFmpeg 7.1.5 with Raspberry Pi's rpi_29 patch, configured from codecs.list
# and nothing else. Then the worker and the tests (Makefile).
#
#   sh build.sh            x86-64 or aarch64, whichever this is
#   JOBS=3 sh build.sh     -j (default 4; the Pi pool has 3 cores)
#   ACM_CACHE, ACM_BUILD   the sources (default ./cache) and the tree (./build/<arch>)
#
# ARM64 builds run natively on the Pi pool (amicradle-teams tools/pibuild.sh),
# never under qemu. On a host without libudev-dev or libva-dev, the pinned
# headers stand in and the host's own libudev.so.1 / libva.so.2 are linked.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCH=$(uname -m)
case "$ARCH" in x86_64|aarch64) ;; *) echo "build: $ARCH is not a target" >&2; exit 2;; esac
CACHE=${ACM_CACHE:-"$HERE/cache"}
B=${ACM_BUILD:-"$HERE/build/$ARCH"}
P="$B/prefix"
JOBS=${JOBS:-4}
LIBDIR=/usr/lib/$ARCH-linux-gnu
mkdir -p "$B" "$P/include" "$P/lib/pkgconfig"
sh "$HERE/fetch.sh"
export PKG_CONFIG_PATH="$P/lib/pkgconfig:$P/lib/$ARCH-linux-gnu/pkgconfig"
log() { echo "build: $*" >&2; }
stamp() { [ -f "$B/.$1.done" ]; }
done_() { : > "$B/.$1.done"; }

# meson: the host's, else a private one (daletop has none)
MESON=$(command -v meson || true)
if [ -z "$MESON" ]; then
    [ -x "$B/venv/bin/meson" ] || { python3 -m venv "$B/venv" && "$B/venv/bin/pip" -q install 'meson==1.10.1'; }
    MESON="$B/venv/bin/meson"
fi

# --- dav1d (BSD-2) ---
if ! stamp dav1d; then
    log "dav1d 1.5.4"
    rm -rf "$B/dav1d-1.5.4"; tar xf "$CACHE/dav1d-1.5.4.tar.xz" -C "$B"
    (cd "$B/dav1d-1.5.4" && "$MESON" setup build --prefix="$P" --libdir=lib --buildtype=release \
        --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false \
        -Dlogging=false >/dev/null && ninja -C build -j "$JOBS" >/dev/null && ninja -C build install >/dev/null)
    done_ dav1d
fi

# --- libopenmpt (BSD-3), no external codecs, no player ---
if ! stamp openmpt; then
    log "libopenmpt 0.8.10"
    rm -rf "$B/libopenmpt-0.8.10+release.autotools"; tar xf "$CACHE/libopenmpt-0.8.10+release.autotools.tar.gz" -C "$B"
    (cd "$B/libopenmpt-0.8.10+release.autotools" && ./configure -q --prefix="$P" --libdir="$P/lib" \
        --enable-static --disable-shared --disable-openmpt123 --disable-examples --disable-tests \
        --disable-doxygen-doc --without-zlib --without-mpg123 --without-ogg --without-vorbis \
        --without-vorbisfile --without-portaudio --without-portaudiocpp --without-pulseaudio \
        --without-sndfile --without-flac >/dev/null && make -s -j "$JOBS" >/dev/null && make -s install >/dev/null)
    done_ openmpt
fi
# a static libopenmpt needs the C++ library and libm; its .pc does not say so
sed -i 's/^Libs.private:.*/Libs.private: -lstdc++ -lm/' "$P/lib/pkgconfig/libopenmpt.pc"

# --- header stand-ins for hosts without the -dev packages ---
shim_pc() {   # name, lib, sofile
    ln -sf "$LIBDIR/$3" "$P/lib/lib$2.so"
    printf 'prefix=%s\nlibdir=%s/lib\nincludedir=%s/include\nName: %s\nDescription: stand-in (ACMedia build.sh)\nVersion: %s\nLibs: -L${libdir} -l%s\nCflags: -I${includedir}\n' \
        "$P" "$P" "$P" "$1" "$4" "$2" > "$P/lib/pkgconfig/$1.pc"
}
EXTRA=""
if [ "$ARCH" = x86_64 ] && ! pkg-config --exists libva 2>/dev/null; then
    log "libva 2.23.0 headers (host libva.so.2)"
    rm -rf "$B/libva-2.23.0"; tar xzf "$CACHE/libva-2.23.0.tar.gz" -C "$B"
    mkdir -p "$P/include/va"
    cp "$B/libva-2.23.0"/va/va*.h "$B/libva-2.23.0"/va/drm/va_drm.h "$P/include/va/"
    sed -e 's/@VA_API_MAJOR_VERSION@/1/; s/@VA_API_MINOR_VERSION@/23/; s/@VA_API_MICRO_VERSION@/0/; s/@VA_API_VERSION@/1.23.0/' \
        "$B/libva-2.23.0/va/va_version.h.in" > "$P/include/va/va_version.h"
    shim_pc libva va libva.so.2 1.23.0
    shim_pc libva-drm va-drm libva-drm.so.2 1.23.0
fi
if [ "$ARCH" = aarch64 ] && ! pkg-config --exists libudev 2>/dev/null; then
    log "libudev.h (systemd v259; host libudev.so.1)"
    cp "$CACHE/libudev-v259.h" "$P/include/libudev.h"
    shim_pc libudev udev libudev.so.1 259
fi

# --- FFmpeg 7.1.5 + rpi_29, LGPL, static ---
list() {   # kind -> comma list for this arch
    awk -v k="$1" -v a="$ARCH" '!/^#/ && NF==2 && ($1==k || $1==k"-"a) {printf "%s%s", s, $2; s=","}' "$HERE/codecs.list"
}
if ! stamp ffmpeg; then
    log "FFmpeg 7.1.5 + rpi_29"
    rm -rf "$B/ffmpeg-7.1.5"; tar xJf "$CACHE/ffmpeg-7.1.5.tar.xz" -C "$B"
    (cd "$B/ffmpeg-7.1.5" && patch -p1 -s < "$CACHE/ffmpeg-7.1.5-rpi_29.patch")
    HW=""
    if [ "$ARCH" = x86_64 ]; then
        HW="--enable-vaapi --enable-libdrm"
    else
        HW="--enable-libdrm --enable-libudev --enable-v4l2-m2m --enable-v4l2-request --enable-sand"
    fi
    (cd "$B/ffmpeg-7.1.5" && ./configure --prefix="$P" --pkg-config-flags=--static \
        --extra-cflags="-I$P/include" --extra-ldflags="-L$P/lib" \
        --disable-everything --disable-autodetect --disable-programs --disable-doc \
        --disable-network --disable-protocols --disable-devices --disable-filters \
        --disable-encoders --disable-muxers --disable-bsfs --disable-hwaccels \
        --enable-static --disable-shared --enable-pic \
        --enable-swscale --enable-swresample --enable-zlib --enable-pthreads \
        --enable-libdav1d --enable-libopenmpt $HW \
        --enable-decoder="$(list decoder)" --enable-demuxer="$(list demuxer)" \
        --enable-parser="$(list parser)" --enable-bsf="$(list bsf)" \
        --enable-hwaccel="$(list hwaccel)" > "$B/ffmpeg-configure.log" 2>&1) \
        || { tail -20 "$B/ffmpeg-configure.log" >&2; exit 1; }
    # LGPL only: refuse a configuration that took GPL, version 3 or nonfree
    if grep -qE '^CONFIG_(GPL|VERSION3|NONFREE)=yes' "$B/ffmpeg-7.1.5/ffbuild/config.mak"; then
        echo "build: FFmpeg configured GPL, version3 or nonfree: refused" >&2; exit 1
    fi
    (cd "$B/ffmpeg-7.1.5" && make -s -j "$JOBS" >/dev/null && make -s install >/dev/null)
    done_ ffmpeg
fi
log "FFmpeg in $P"
make -C "$HERE" ARCH="$ARCH" PREFIX="$P" OUT="$B" -j "$JOBS"
