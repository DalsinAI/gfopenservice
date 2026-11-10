#!/bin/sh
# ACMedia's benchmark set (design section 8): 1080p H.264, HEVC and AV1, and
# 4K HEVC 8- and 10-bit, in software and on the hardware decoder where there
# is one, flat out and paced at the film's rate (frame-time jitter, drops).
# On the Pi pool run it with --bench (amicradle-teams tools/pibuild.sh), so
# nobody else's build spoils the numbers. The files come from
# BENCH=DIR sh tests/gen_clips.sh.
#   sh tests/bench.sh BENCHDIR [OUT]
# MIT, Copyright (c) 2026 AmiCradle Team.
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
D=$1
OUT=${2:-"$HERE/build/$(uname -m)"}
T="$OUT/acm_test"
echo "host $(uname -m), $(nproc) cores, load $(cut -d' ' -f1-3 /proc/loadavg)"
grep -m1 -E "model name|Model" /proc/cpuinfo 2>/dev/null
b() { echo "-- $*"; "$T" bench "$@" 2>&1 | sed 's/^/   /'; }
for f in h264-1080p30.mkv hevc-1080p30.mkv av1-1080p30.mkv; do
    b "$D/$f" --n 300
    b "$D/$f" --n 300 --realtime
done
b "$D/h264-1080p30.mkv" --n 300 --copy
for f in hevc-1080p30.mkv hevc-2160p30.mkv hevc10-2160p30.mkv h264-1080p30.mkv; do
    b "$D/$f" --n 300 --hw --dmabuf
    b "$D/$f" --n 300 --hw --dmabuf --realtime
    b "$D/$f" --n 300 --hw
done
b "$D/hevc-2160p30.mkv" --n 150
b "$D/hevc10-2160p30.mkv" --n 150
echo "load after $(cut -d' ' -f1-3 /proc/loadavg)"
