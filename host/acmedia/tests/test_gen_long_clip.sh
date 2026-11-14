#!/bin/sh
# Fast contract test for gen_long_clip.sh. It uses command shims so the default
# 60-second option is tested without manufacturing a long film in every suite.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TMP=${TMPDIR:-/tmp}/acmedia-long-clip-test.$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/bin" "$TMP/out"

cat > "$TMP/bin/ffmpeg" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$FAKE_ARGS"
for last do :; done
: > "$last"
EOF
cat > "$TMP/bin/ffprobe" <<'EOF'
#!/bin/sh
case " $* " in
    *' format=duration '*) printf '%s.000000\n' "$FAKE_SECONDS" ;;
    *' stream=codec_name '*) case " $* " in *' a:0 '*) echo aac;; *) echo h264;; esac ;;
    *' stream=width '*) echo 640 ;;
    *' stream=height '*) echo 360 ;;
    *' stream=r_frame_rate '*) echo 30/1 ;;
    *' stream=nb_frames '*) echo $((FAKE_SECONDS * 30)) ;;
    *' stream=sample_rate '*) echo 48000 ;;
    *' stream=channels '*) echo 2 ;;
    *) echo "unexpected ffprobe arguments: $*" >&2; exit 1 ;;
esac
EOF
cat > "$TMP/bin/sha256sum" <<'EOF'
#!/bin/sh
printf '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  %s\n' "$1"
EOF
chmod +x "$TMP/bin/ffmpeg" "$TMP/bin/ffprobe" "$TMP/bin/sha256sum"

# Default contract: exactly 60 seconds and an untracked generated-media path.
FAKE_SECONDS=60 FAKE_ARGS="$TMP/args" PATH="$TMP/bin:$PATH" \
    FFMPEG=ffmpeg FFPROBE=ffprobe SHA256SUM=sha256sum \
    sh "$HERE/gen_long_clip.sh" --output "$TMP/out/default.mp4" > "$TMP/default.receipt"
grep -qx -- '-t' "$TMP/args"
pair()
{
    awk -v first="$1" -v second="$2" 'previous == first && $0 == second { found=1 } { previous=$0 } END { exit !found }' "$TMP/args"
}
pair -t 60
pair -i testsrc2=s=640x360:r=30
pair -i sine=f=330:r=48000
pair -c:v libx264
pair -pix_fmt yuv420p
pair -g 30
pair -c:a aac
pair -ar 48000
pair -ac 2
pair -f mp4
grep -qx 'long-clip: duration=60.000000 seconds' "$TMP/default.receipt"
grep -qx 'long-clip: video=h264 640x360 30/1 frames=1800' "$TMP/default.receipt"
grep -qx 'long-clip: audio=aac 48000Hz channels=2' "$TMP/default.receipt"
grep -qx '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  default.mp4' "$TMP/out/default.mp4.sha256"

# The short override is for a quick local recipe smoke test; all media options
# remain the same and the expected frame count follows its duration.
FAKE_SECONDS=3 FAKE_ARGS="$TMP/args" PATH="$TMP/bin:$PATH" \
    FFMPEG=ffmpeg FFPROBE=ffprobe SHA256SUM=sha256sum \
    sh "$HERE/gen_long_clip.sh" --seconds 3 --output "$TMP/out/short.mp4" > "$TMP/short.receipt"
grep -qx 'long-clip: duration=3.000000 seconds' "$TMP/short.receipt"
grep -qx 'long-clip: video=h264 640x360 30/1 frames=90' "$TMP/short.receipt"
grep -qx 'long-clip: file='"$TMP/out/short.mp4" "$TMP/short.receipt"

if sh "$HERE/gen_long_clip.sh" --seconds 0 --output "$TMP/out/bad.mp4" >/dev/null 2>&1; then
    echo "test_gen_long_clip: zero duration accepted" >&2
    exit 1
fi
if sh "$HERE/gen_long_clip.sh" --unknown >/dev/null 2>&1; then
    echo "test_gen_long_clip: unknown option accepted" >&2
    exit 1
fi
sh "$HERE/gen_long_clip.sh" --help | grep -q '^usage:'
echo "test_gen_long_clip: pass"
