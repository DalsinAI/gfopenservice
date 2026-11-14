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
| `tests/gen_clips.sh`, `tests/gen_amiga.py`, `tests/clips/` | The test clips (CDXL, ILBM, 8SVX and the module are written by `gen_amiga.py` from the formats' definitions) |

## Building

    sh build.sh                     # x86-64 on daletop, under the DevDrive
    JOBS=3 sh build.sh              # aarch64, natively on the Pi pool:
    tools/pibuild.sh --name acmedia <this folder> -- env JOBS=3 sh build.sh   # (amicradle-teams)

The output is in `build/<arch>/`: `acmedia-worker`, `acmedia-worker-test` (the
same worker with the test hooks: crash, hang, leak, fail the hardware, try to
escape) and `acm_test`. ARM64 is never built under qemu.

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

Our code here is MIT, as the rest of gfopenservice. The worker links:

- FFmpeg 7.1.5, LGPL 2.1 or later; `build.sh` refuses `--enable-gpl`,
  `--enable-version3` and `--enable-nonfree`;
- Raspberry Pi's patch to it: its files are MIT, BSD or LGPL; its one GPL
  file, a checkasm test, is not built;
- dav1d (BSD-2-Clause) and libopenmpt (BSD-3-Clause);
- from the host, dynamically: libva and libdrm (MIT), and libudev (LGPL 2.1).

FFmpeg is linked statically. To relink it with another FFmpeg, as LGPL 2.1
section 6 allows, build that FFmpeg into `build/<arch>/prefix` and run
`make`: this folder is the worker's complete source, and `build.sh` is how it
is made.
