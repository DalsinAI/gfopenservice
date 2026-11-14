#!/bin/sh
# Generate the long A/V fixture used for ACMedia's interactive Pi receipts.
# The generated media and its hash receipt are deliberately not kept in git.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
seconds=60
out="$HERE/generated/h264-av-60s.mp4"

usage()
{
    cat <<EOF
usage: $0 [--seconds N] [--output FILE]

Generate a 640x360, 30 fps H.264/AAC test film (60 seconds by default),
verify its streams and duration with ffprobe, and write FILE.sha256.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) [ $# -ge 2 ] || { usage >&2; exit 2; }; seconds=$2; shift 2 ;;
        --output)  [ $# -ge 2 ] || { usage >&2; exit 2; }; out=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "gen_long_clip: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done
case "$seconds" in *[!0-9]*|'') echo "gen_long_clip: --seconds must be a positive integer" >&2; exit 2;; esac
[ "$seconds" -gt 0 ] || { echo "gen_long_clip: --seconds must be a positive integer" >&2; exit 2; }

FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}
SHA256SUM=${SHA256SUM:-sha256sum}
for tool in "$FFMPEG" "$FFPROBE" "$SHA256SUM"; do
    command -v "$tool" >/dev/null 2>&1 || { echo "gen_long_clip: missing tool: $tool" >&2; exit 1; }
done

dir=$(dirname -- "$out")
base=$(basename -- "$out")
mkdir -p "$dir"
tmp="$dir/.$base.tmp.$$"
trap 'rm -f "$tmp"' EXIT HUP INT TERM

"$FFMPEG" -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=s=640x360:r=30" \
    -f lavfi -i "sine=f=330:r=48000" \
    -t "$seconds" -map_metadata -1 \
    -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 30 \
    -c:a aac -ar 48000 -ac 2 -shortest -movflags +faststart \
    -f mp4 "$tmp"

probe()
{
    "$FFPROBE" -v error "$@" -of default=noprint_wrappers=1:nokey=1 "$tmp"
}
duration=$(probe -show_entries format=duration)
vcodec=$(probe -select_streams v:0 -show_entries stream=codec_name)
width=$(probe -select_streams v:0 -show_entries stream=width)
height=$(probe -select_streams v:0 -show_entries stream=height)
rate=$(probe -select_streams v:0 -show_entries stream=r_frame_rate)
frames=$(probe -select_streams v:0 -show_entries stream=nb_frames)
acodec=$(probe -select_streams a:0 -show_entries stream=codec_name)
sample_rate=$(probe -select_streams a:0 -show_entries stream=sample_rate)
channels=$(probe -select_streams a:0 -show_entries stream=channels)
expected_frames=$((seconds * 30))

awk -v got="$duration" -v want="$seconds" 'BEGIN { d = got - want; if (d < 0) d = -d; exit(d > 0.001) }' || {
    echo "gen_long_clip: duration $duration, expected $seconds seconds" >&2; exit 1;
}
[ "$vcodec" = h264 ] && [ "$width" = 640 ] && [ "$height" = 360 ] && \
    [ "$rate" = 30/1 ] && [ "$frames" = "$expected_frames" ] || {
    echo "gen_long_clip: video is $vcodec ${width}x${height} $rate, $frames frames; expected h264 640x360 30/1, $expected_frames frames" >&2
    exit 1
}
[ "$acodec" = aac ] && [ "$sample_rate" = 48000 ] && [ "$channels" = 2 ] || {
    echo "gen_long_clip: audio is $acodec ${sample_rate}Hz ${channels}ch; expected aac 48000Hz 2ch" >&2
    exit 1
}

mv "$tmp" "$out"
trap - EXIT HUP INT TERM
(cd "$dir" && "$SHA256SUM" "$base" > "$base.sha256")
hash=$(awk '{print $1}' "$out.sha256")
printf 'long-clip: duration=%s seconds\n' "$duration"
printf 'long-clip: video=%s %sx%s %s frames=%s\n' "$vcodec" "$width" "$height" "$rate" "$frames"
printf 'long-clip: audio=%s %sHz channels=%s\n' "$acodec" "$sample_rate" "$channels"
printf 'long-clip: sha256=%s\n' "$hash"
printf 'long-clip: file=%s\n' "$out"
printf 'long-clip: hash-receipt=%s.sha256\n' "$out"
