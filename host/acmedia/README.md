# ACMedia, stage a: our own FFmpeg in a sandboxed worker

The host side of ACMedia (amicradle-teams `designs/ACMEDIA-DESIGN-20261110.md`).
The Amiga asks for a film or a sound. A sandboxed host process decodes it with
an FFmpeg we build and pin ourselves, in hardware where the host has a
decoder. Stage a is that process, its build and its tests. The guest-facing
ABI (hostlib ids `$00012000` up, `akikoplus.device`) comes in stage b, built
on `src/acm_proto.h`. No FFmpeg type ever crosses that line.

| File | |
|---|---|
| `PINS`, `fetch.sh` | The pinned sources, each checked by sha256 (and FFmpeg's GPG signature): FFmpeg 7.1.5, Raspberry Pi's `ffmpeg-7.1.5-rpi_29.patch`, dav1d 1.5.4, libopenmpt 0.8.10, libva 2.23.0's headers (x86-64), libudev.h from systemd v259 (for hosts without libudev-dev) |
| `codecs.list` | Everything FFmpeg is built with: about 80 decoders, the demuxers, parsers and bitstream filters, and the hardware decoders per architecture. Nothing else is enabled |
| `build.sh` | dav1d, libopenmpt, then FFmpeg (static, LGPL only: it refuses a GPL, version 3 or nonfree configuration), then `make` |
| `src/acm_worker.c` | `acmedia-worker`: one session, one process |
| `src/acm_sandbox.c` | The worker's lock-down, applied before it reads its first message |
| `src/acm_caps.c` | The capability probe: V4L2 memory-to-memory decoders and VAAPI profiles |
| `src/acm_broker.c` | The broker's side: start, check the lock-down, ask with a watchdog, give the frame pool. The runtime uses it in stage b |
| `src/acm_proto.h` | The worker protocol, with ACMedia's error codes and codec tags (`acm_codecs.h`) |
| `tests/acm_test.c`, `tests/run_tests.sh` | The host test harness |
| `src/acm_plane.c`, `src/acvideo_proto.h` | The plane route (design 3.2): dma-buf frames to acscreen's video socket, frames sent back unshown returned for compositing (Team 6; the protocol is amicradle `appliance/acscreen/acvideo_proto.h`) |
| `tests/acm_plane_test.c` | The plane route against a stand-in acscreen (no worker, FFmpeg or display) |
| `tests/acm_planetest.c` | M4K step 2: the worker's dma-bufs on a KMS overlay plane (`--test-only`, `--writeback`, `--acscreen`); built where libdrm is |
| `src/acm_runtime.c` → `libacmedia.so` | M4K step 4: the runtime's backend for the twelve ids (`acmedia_backend_v1`, `src/acmedia_service.h`, a copy of gfosmotherboards'). A worker per session and per probe, frames to acscreen's plane or to ACM_FRAME; no FFmpeg (Team 6) |
| `src/acm_guest.h` | The guest ABI's numbers as the backend answers them, and the CAPS, INFO and RESULT block layouts (to go into amicradle-guest's `hostlib/acmedia/acmedia.h`) |
| `tests/acm_runtime_test.c`, `tests/acm_fakeworker.c` | `libacmedia.so` loaded as the runtime loads it, against a stand-in worker (the protocol, made-up frames, no FFmpeg) and a stand-in acscreen |
| `tests/gen_clips.sh`, `tests/gen_amiga.py`, `tests/clips/` | The test clips (CDXL, ILBM, 8SVX and the module are written by `gen_amiga.py` from the formats' definitions) |

## Building

The portable worker uses the host's system FFmpeg dynamically. Decoder
libraries stay host packages; they are not copied into GFOS, a pack or a ROM:

    make system                    # acmedia-worker-system and libacmedia.so

The pinned build remains the Pi hardware-validation build because it carries
Raspberry Pi's V4L2 request decoder:

    sh build.sh                     # x86-64 on daletop, under the DevDrive
    JOBS=3 sh build.sh              # aarch64, natively on the Pi pool:
    tools/pibuild.sh --name acmedia <this folder> -- env JOBS=3 sh build.sh   # (amicradle-teams)

The output is in `build/<arch>/`: `acmedia-worker`, `acmedia-worker-test` (the
same worker with the test hooks: crash, hang, leak, fail the hardware, try to
escape) and `acm_test`. ARM64 is never built under qemu.

## libacmedia.so, the runtime's backend

a1200native loads `libacmedia.so` before `jit_init` (gfosmotherboards
`a1200-x86/native/acmedia_event.c`: `$AC_ACMEDIA_DIR`, `openservice/` beside
the emulator, or this folder's `build/<arch>/` in the dev tree) and calls
`acmedia_backend_v1(host)`. With no library, or no `acmedia-worker` beside it
(`AC_ACMEDIA_WORKER` overrides), the ids stay off and akikoplus.device takes
rung B. The runtime's broker checks the guest's ranges and owns the handles
and the event queue; the backend gets host pointers and posts events.

- ACM_PROBE and ACM_OPEN get the source as an fd from the runtime (ACFS:
  opened beneath the unit's share), answer at once, and start a thread that
  runs the worker; PROBED, OPENED or ERROR follows. Nothing waits for a
  decoder on the emulation thread.
- ACM_CONTROL queues PLAY, PAUSE, STOP, SEEK, RATE, SETRECT for the session's
  thread; EVENTS and VOLUME are kept. Frames are paced by their pts from the
  moment PLAY lands.
- PLANE: frames go to acscreen (`$AC_VIDEO_SOCK`) as dma-bufs a frame's time
  ahead of their vblank. No acscreen, or acscreen turning the plane off: RECT,
  and the frames go to ACM_FRAME (route GUEST) until the RTG surface (stage c).
- ACM_FRAME gives the newest due frame once, as YUV420 at its own size; other
  formats and sizes are ACM_E_FORMAT until stage c.
- Video and sound use one worker session. ACM_AUDIO copies raw interleaved
  big-endian S16 for the Amiga-side consumer and returns the exact byte count;
  PTS/position is obtained separately through INFO and events. Direct ACAHI output is not
  claimed by this step. Feeds, the CD, SELECT and TARGET are not in this step.
- The policy: `AC_ACMEDIA_DENY`, `AC_ACMEDIA_HWONLY` (tags, comma separated),
  `AC_ACMEDIA_HW=0`. CAPS reports it, and the host's hardware decoders once a
  worker has said what they are.

`make runtime` builds `libacmedia.so`, `acm_fakeworker`, `acm_runtime_test`
and `acm_plane_test`, none of which needs FFmpeg:

    make runtime && build/$(uname -m)/acm_runtime_test build/$(uname -m)/libacmedia.so build/$(uname -m)/acm_fakeworker

The real-file receipt runs the same backend against the real worker and the
checked-in H.264/AAC clip:

    build/$(uname -m)/acm_runtime_test build/$(uname -m)/libacmedia.so \
        build/$(uname -m)/acmedia-worker-system --real "$PWD/tests/clips/h264-av.mp4"

## The worker

`acmedia-worker [--fd N] [--hw] [--mem MB]`. The broker starts it with a
socketpair on fd 3, an empty environment and stdin on /dev/null. In order:

1. With `--hw`, it opens the decoder devices: VAAPI's render node on x86-64;
   on ARM64, the DRM device, then it looks for the V4L2 stateless HEVC
   decoder (the Pi 5's `rpi-hevc-dec`, the Pi 4's `rpivid`) and the stateful
   H.264 one (the Pi 4's `bcm2835-codec`).
2. It locks itself down, before reading a single byte:
   - no new privileges, `RLIMIT_AS`, `RLIMIT_NOFILE` and `RLIMIT_CORE` 0;
   - Landlock: no filesystem except the decoder devices (read and write), and
     `/sys` and `/run/udev` read-only for the V4L2 device scan; no TCP; no
     signals or abstract sockets outside its own domain;
   - a seccomp-BPF allow-list that kills the process on anything else: no
     `socket`, `execve` or `fork` (threads only), no executable memory, and
     `ioctl` for V4L2, media, DRM, dma-buf and dma-heap only.
3. The broker reads `/proc/PID/status` (`Seccomp: 2`, `NoNewPrivs: 1`) and
   refuses a worker that is not locked down before it sends anything.

The source arrives as one read-only fd. FFmpeg reads it through our own
AVIOContext: no protocol is compiled in, `file` included.

Software decoders decode straight into the frame pool, a sealed memfd the
broker makes, so a frame is not copied between processes (`ACM_FF_COPIED`
marks the ones that were). Hardware frames go as dma-buf fds
(`ACM_WANT_DMABUF`, for the KMS plane), or are read back into the pool.

A hardware decoder that fails mid-stream is replaced by the software one at
the last keyframe; frames already shown are skipped. The broker sees only
`ACM_FF_PATH` on the next frame.

The policy (allowed codecs, codecs allowed only on hardware) comes in HELLO.
A denied codec is `ACM_E_DENIED` before any decoder opens, and probe marks
the stream.

## Tests

    sh tests/run_tests.sh           # all of them, on this host
    build/<arch>/acm_test bench FILE [--hw] [--copy] [--dmabuf] [--realtime]

`run_tests.sh` runs these:

- the protocol's good and bad requests, and every clip: probe, frames to the
  end, the end twice, seek, the same frames again;
- the policy: a deny never opens a decoder;
- crash, hang and leak: each ends as `ACM_E_DECODER` and the next session
  plays;
- the sandbox: `/etc/passwd`, a write to `/tmp` and a listing of `/home`
  refused, and `socket`, `execve`, `fork`, executable memory and a signal to
  the broker each killed by seccomp;
- corrupted files through probe, open and decode;
- where a hardware decoder takes a clip: every frame's checksum against
  software's, and the fallback mid-stream.

## Licences

Our code here is MIT, as the rest of gfopenservice. `make system` dynamically
links the host's libavformat, libavcodec, libswresample, libswscale and
libavutil. The host package's build configuration and licence are the receipt
for that worker; no FFmpeg binary or source is copied into this repository,
GFOS, a pack or a ROM.

The pinned Pi validation worker links:

- FFmpeg 7.1.5, LGPL 2.1 or later; `build.sh` refuses `--enable-gpl`,
  `--enable-version3` and `--enable-nonfree`;
- Raspberry Pi's patch to it: most files are MIT, BSD or LGPL. Its HEVC v1-v3
  compatibility headers and checkasm test are GPL-2.0. `build.sh` records
  `GPL-2.0` when `CONFIG_V4L2_REQ_HEVC_VX=yes`, otherwise
  `LGPL-2.1-or-later`, in `ffmpeg-result-licence`;
- dav1d (BSD-2-Clause) and libopenmpt (BSD-3-Clause);
- from the host, dynamically: libva and libdrm (MIT), and libudev (LGPL 2.1).

The pinned FFmpeg is linked statically. To relink it with another FFmpeg, as LGPL 2.1
section 6 allows, build that FFmpeg into `build/<arch>/prefix` and run
`make`: this folder is the worker's complete source, and `build.sh` is how it
is made. A distributed pinned worker must be accompanied by the corresponding
FFmpeg and patch source, notices and the relink path; a result that selected
the GPL compatibility objects is distributed under GPL-2.0 terms.
