#!/bin/sh
# Real x86-64 ACMedia proof: distro FFmpeg worker, backend, guest video/audio,
# ACAHI shared ring through the real C mixer, and the software video socket.
# Usage: run_x86_runtime.sh ACAUDIOMIXER [OUT]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MIXER=${1:?usage: run_x86_runtime.sh ACAUDIOMIXER [OUT]}
OUT=${2:-"$HERE/build/x86_64"}
[ "$(uname -m)" = x86_64 ] || { echo "run_x86_runtime: x86-64 only" >&2; exit 2; }
[ -x "$MIXER" ] || { echo "run_x86_runtime: no mixer at $MIXER" >&2; exit 2; }

make -C "$HERE" ARCH=x86_64 OUT="$OUT" x86-runtime
R="$OUT/runtime"
CLIP="$HERE/tests/clips/h264-av.mp4"
ldd "$R/acmedia-worker" | tee "$OUT/x86-worker-ldd.txt"
! grep -q 'not found' "$OUT/x86-worker-ldd.txt"

"$OUT/acm_runtime_test" "$R/libacmedia.so" "$R/acmedia-worker" --real "$CLIP"
AC_MIXER_EXE="$MIXER" "$OUT/acm_runtime_test" \
    "$R/libacmedia.so" "$R/acmedia-worker" --real-acahi "$CLIP"
"$OUT/acm_runtime_test" "$R/libacmedia.so" "$R/acmedia-worker" \
    --real-feed "$HERE/tests/clips/h264-small.mkv"
"$OUT/acm_runtime_test" "$R/libacmedia.so" "$R/acmedia-worker" \
    --real-seek-feed "$HERE/tests/clips/h264-small.mkv"
"$OUT/acm_runtime_test" "$R/libacmedia.so" "$R/acmedia-worker" \
    --real-cd "$CLIP"

if [ -x "$OUT/acm_planetest" ]; then
    python3 "$HERE/tests/test_planetest_copy.py" \
        "$OUT/acm_planetest" "$R/acmedia-worker" "$CLIP"
else
    echo "run_x86_runtime: video COPY route skipped (libdrm development files unavailable)"
fi
echo "run_x86_runtime: PASS (real system FFmpeg worker; no stub worker)"
