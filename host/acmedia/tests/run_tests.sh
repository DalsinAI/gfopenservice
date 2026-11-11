#!/bin/sh
# ACMedia stage a's tests, on this host (x86-64 or, through the Pi pool,
# aarch64): conformance, policy, crash, sandbox, fuzz, and with a hardware
# decoder here the hardware/software comparison and the fallback.
#   sh tests/run_tests.sh [OUT]       OUT: the build folder (default build/<arch>)
#   FUZZ=N                            corrupted files to try (default 300)
# MIT, Copyright (c) 2026 AmiCradle Team.
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$HERE/build/$(uname -m)"}
T="$OUT/acm_test"
C="$HERE/tests/clips"
fail=0
run() { echo "== $*"; "$T" "$@" || { echo "FAILED: $*"; fail=$((fail + 1)); }; }
run caps
run conformance "$C"
run policy "$C/h264-small.mkv"
run crash "$C/h264-small.mkv"
run sandbox
run sandbox --hw
run fuzz "$C" "${FUZZ:-300}"
# hardware against software, where a hardware decoder takes the file
for f in h264-av.mp4 hevc-av.mp4 hevc-10bit.mkv mpg2-small.mpg; do
    sw=$("$T" hash "$C/$f" --quiet | awk '/^frames/ {print $NF}')
    hwline=$("$T" hash "$C/$f" --hw --quiet | grep -E '^(path|frames)' | tr '\n' ' ')
    hw=$(echo "$hwline" | awk '{print $NF}')
    case "$hwline" in
        *"path software"*) echo "== $f: no hardware decoder here (software both ways)";;
        *) if [ "$sw" = "$hw" ]; then echo "== $f: hardware frames = software frames ($sw): pass"
           elif [ "$f" = mpg2-small.mpg ]; then echo "== $f: hardware and software differ, as MPEG-2's IDCT allows (compared with a tolerance in stage d)"
           else echo "== $f: hardware $hw, software $sw: FAILED ($hwline)"; fail=$((fail + 1)); fi;;
    esac
done
run fallback "$C/hevc-av.mp4"
run fallback "$C/h264-av.mp4"
echo "run_tests: $fail failed"
[ $fail = 0 ]
