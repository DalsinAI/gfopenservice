#!/bin/sh
# Makes ACMedia's test clips (tests/clips, kept in git, a few hundred KB) and
# the benchmark files (not kept: BENCH=DIR). The clips' encoders are the
# host's own ffmpeg (any build with libx264, libx265, libvpx, libsvtav1); they
# only make test data. The Amiga files come from gen_amiga.py.
#   sh gen_clips.sh                 tests/clips
#   BENCH=/some/dir sh gen_clips.sh the 1080p and 4K benchmark files too
# File names start with our codec tag, which the conformance test checks.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
C="$HERE/clips"
mkdir -p "$C"
F="ffmpeg -hide_banner -loglevel error -y"
SRC="testsrc2=s=320x180:r=30"
mk() { [ -f "$C/$1" ] || { shift_name=$1; shift; $F "$@" "$C/$shift_name"; }; }
mk h264-small.mkv   -f lavfi -i "$SRC" -t 1 -c:v libx264 -pix_fmt yuv420p -g 15
mk hevc-small.mkv   -f lavfi -i "$SRC" -t 1 -c:v libx265 -pix_fmt yuv420p -x265-params log-level=error:keyint=15
mk hevc-10bit.mkv   -f lavfi -i "$SRC" -t 1 -c:v libx265 -pix_fmt yuv420p10le -x265-params log-level=error:keyint=15
mk av1-small.mkv    -f lavfi -i "$SRC" -t 1 -c:v libsvtav1 -pix_fmt yuv420p -g 15 -svtav1-params verbosity=0
mk vp9-small.webm   -f lavfi -i "$SRC" -t 1 -c:v libvpx-vp9 -pix_fmt yuv420p -g 15
mk vp8-small.webm   -f lavfi -i "$SRC" -t 1 -c:v libvpx -pix_fmt yuv420p -g 15
mk mpg2-small.mpg   -f lavfi -i "$SRC" -t 1 -c:v mpeg2video -g 15
mk mpg4-small.avi   -f lavfi -i "$SRC" -t 1 -c:v mpeg4 -g 15
mk mjpg-small.avi   -f lavfi -i "$SRC" -t 1 -c:v mjpeg
mk flac-tone.flac   -f lavfi -i "sine=f=440:r=44100" -t 1 -c:a flac
mk opus-tone.ogg    -f lavfi -i "sine=f=440:r=48000" -t 1 -c:a libopus
mk vorb-tone.ogg    -f lavfi -i "sine=f=440:r=44100" -t 1 -c:a libvorbis
# a film with sound, 640x360, for the hardware paths and the fallback
mk h264-av.mp4      -f lavfi -i "testsrc2=s=640x360:r=30" -f lavfi -i "sine=f=330:r=48000" -t 2 -c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac -shortest
mk hevc-av.mp4      -f lavfi -i "testsrc2=s=640x360:r=30" -f lavfi -i "sine=f=330:r=48000" -t 2 -c:v libx265 -pix_fmt yuv420p -x265-params log-level=error:keyint=30 -c:a aac -shortest
python3 "$HERE/gen_amiga.py" "$C"

if [ -n "${BENCH:-}" ]; then
    mkdir -p "$BENCH"
    N="noise=alls=8:allf=t"
    b() { [ -f "$BENCH/$1" ] || { n=$1; shift; echo "bench file $n" >&2; $F "$@" "$BENCH/$n"; }; }
    b h264-1080p30.mkv  -f lavfi -i "testsrc2=s=1920x1080:r=30,$N" -t 20 -c:v libx264 -preset veryfast -b:v 8M -maxrate 12M -bufsize 16M -g 60 -pix_fmt yuv420p
    b hevc-1080p30.mkv  -f lavfi -i "testsrc2=s=1920x1080:r=30,$N" -t 20 -c:v libx265 -preset ultrafast -b:v 6M -pix_fmt yuv420p -x265-params log-level=error:keyint=60
    b av1-1080p30.mkv   -f lavfi -i "testsrc2=s=1920x1080:r=30,$N" -t 20 -c:v libsvtav1 -preset 10 -b:v 5M -g 60 -pix_fmt yuv420p -svtav1-params verbosity=0
    b hevc-2160p30.mkv  -f lavfi -i "testsrc2=s=3840x2160:r=30,$N" -t 10 -c:v libx265 -preset ultrafast -b:v 40M -pix_fmt yuv420p -x265-params log-level=error:keyint=60
    b hevc10-2160p30.mkv -f lavfi -i "testsrc2=s=3840x2160:r=30,$N" -t 10 -c:v libx265 -preset ultrafast -b:v 40M -pix_fmt yuv420p10le -x265-params log-level=error:keyint=60
fi
