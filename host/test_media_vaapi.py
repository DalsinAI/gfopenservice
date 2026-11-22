"""media.decode and media.encode on the graphics card (OPENSERVICE_HWACCEL=vaapi).

Runs test_media_video.py (every codec, frames in order, backwards and at random, against ffmpeg's own
decoding) and test_media_encode.py (every format, checked by decoding the result) again with the card
asked for, and checks the libraries said where each video went: H.264 and HEVC decoded on the card where
it can, everything else on the CPU, and the results the same. Skipped (exit 0, said so) on a machine with
no VA-API device or driver. MIT, Copyright (c) 2026 AmiCradle Team."""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
node = os.environ.get("OPENSERVICE_VAAPI_DEVICE") or "/dev/dri/renderD128"
drivers = glob.glob("/usr/lib/*/dri/*_drv_video.so") + glob.glob("/usr/lib/dri/*_drv_video.so")
if not os.access(node, os.R_OK | os.W_OK) or not drivers:
    print(f"skipped: no VA-API here ({node} {'usable' if os.access(node, os.R_OK | os.W_OK) else 'not usable'}, "
          f"{len(drivers)} drivers)")
    sys.exit(0)

env = dict(os.environ, OPENSERVICE_HWACCEL="vaapi")
said = []
for test in ("test_media_video.py", "test_media_encode.py"):
    r = subprocess.run([sys.executable, os.path.join(HERE, test)], cwd=HERE, env=env, capture_output=True, text=True,
                       timeout=900)
    assert r.returncode == 0, (test, r.stdout[-2000:], r.stderr[-2000:])
    said += [line for line in r.stderr.splitlines() if line.startswith(("media.decode:", "media.encode:"))]

decoded = {line.split()[1]: line for line in said if line.startswith("media.decode:")}
assert set(decoded) >= {"h264", "hevc"}, said            # each video asked for the card says where it went
on_card = sorted(k for k, v in decoded.items() if "on the graphics card" in v)
print("decoded on the card: " + (", ".join(on_card) or "none (this card decodes none of them)"))
print("decoded on the CPU: " + ", ".join(sorted(set(decoded) - set(on_card))))
films = [line for line in said if line.startswith("media.encode: HEVC film")]
assert films, said                                        # the 640x480 film was offered to the card
print(f"films on the card: {sum('on the graphics card' in f for f in films)} of {len(films)} offered (at least "
      f"256x256; one that decodes wrong or that the card refuses goes to x265); every film checked by test_media_encode")
for line in films:
    print("  " + line.split(": ", 1)[1])
