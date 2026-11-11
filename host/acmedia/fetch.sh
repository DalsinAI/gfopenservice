#!/bin/sh
# Downloads ACMedia's pinned sources (PINS) into ACM_CACHE and checks each
# sha256, and FFmpeg's GPG signature when gpg is there. A file already in the
# cache with the right sum is not fetched again. Copy the cache to a machine
# without the network (a Pi in the pool gets it with the source tree).
#   ACM_CACHE  where the files go (default: host/acmedia/cache)
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CACHE=${ACM_CACHE:-"$HERE/cache"}
mkdir -p "$CACHE"
FPR=FCF986EA15E6E293A5644F10B4322F04D67658D8
grep -v '^#' "$HERE/PINS" | while read -r name file sum url; do
    [ -n "$name" ] || continue
    f="$CACHE/$file"
    if [ "$sum" != "-" ] && [ -f "$f" ] && echo "$sum  $f" | sha256sum -c - >/dev/null 2>&1; then
        continue
    fi
    [ "$sum" = "-" ] && [ -f "$f" ] && continue
    echo "fetch: $file" >&2
    curl -sSfL -o "$f.part" "$url"
    if [ "$sum" != "-" ] && ! echo "$sum  $f.part" | sha256sum -c - >/dev/null 2>&1; then
        echo "fetch: SHA-256 mismatch for $file (kept as $f.part)" >&2
        exit 1
    fi
    mv "$f.part" "$f"
done
if command -v gpg >/dev/null 2>&1; then
    G=$(mktemp -d); chmod 700 "$G"
    if curl -sSfL https://ffmpeg.org/ffmpeg-devel.asc -o "$G/key.asc" && GNUPGHOME="$G" gpg -q --import "$G/key.asc" 2>/dev/null; then
        if GNUPGHOME="$G" gpg --status-fd 1 --verify "$CACHE/ffmpeg-7.1.5.tar.xz.asc" "$CACHE/ffmpeg-7.1.5.tar.xz" 2>/dev/null | grep -q "VALIDSIG $FPR"; then
            echo "fetch: FFmpeg signature good ($FPR)" >&2
        else
            echo "fetch: FFmpeg signature does NOT verify" >&2; rm -rf "$G"; exit 1
        fi
    else
        echo "fetch: FFmpeg key not reachable; sha256 checked only" >&2
    fi
    rm -rf "$G"
fi
echo "fetch: all pinned files in $CACHE" >&2
