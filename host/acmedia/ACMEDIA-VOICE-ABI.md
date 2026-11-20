# ACMedia direct ACAHI voice ABI v1

This is the next-build host boundary between `libacmedia.so` and
ACAudioMixer. It does not change the guest ACAHI register ABI. Its canonical
field offsets are `src/acmedia_voice.h` and the matching
`a1200-x86/native/acmedia_voice.h` in gfosmotherboards PR #160.

For `ACMG_OUT_ACAHI`, the producer creates
`/dev/shm/amicradle-acmedia-PORT-HANDLE.acmv` (or the directory named by
`AC_MIXER_RINGS`), mode 0600, with a 128-byte little-endian header and a
power-of-two stereo-frame ring. The stream is S16LE, stereo, 44.1 kHz. The
worker resamples to that exact contract only when the direct route is open.

The producer writes samples before publishing `WRITE` with release ordering
and never lets `WRITE - READ` exceed `CAPACITY`. The mixer publishes `READ`,
`PLAYED` and `LATENCY`. Media time is:

```
ORIGIN_NS + PLAYED * 1,000,000,000 / 44,100
```

Field ownership after publication is explicit: the producer owns `FLAGS`,
`WRITE`, `ORIGIN_NS`, `GAIN_Q16` and `GENERATION`; the mixer owns `READ`,
`PLAYED` and `LATENCY`. The header dimensions are immutable. The producer
uses its compiled capacity and does not trust the writable mapping for bounds.

The runtime uses the latency-corrected value as the audio master. It admits
video up to 40 ms ahead, waits beyond that bound, and drops a video frame over
100 ms late. After audio EOF drains, picture continues on wall time from the
last played position. If neither mixer cursor moves for two complete one-second
discovery intervals, the runtime commits that session to the same picture-only
wall clock and drains but no longer publishes sound. This bounded fallback is
for an absent or pre-v1 mixer; the device should select direct ACAHI only when
the matching mixer is available. These are queue bounds, not a
physical-latency claim.

PLAY clears `PAUSED`; PAUSE sets it. `GAIN_Q16` is CONTROL VOLUME. Non-1x RATE
is refused because this v1 voice and its mixer clock are fixed at 44.1 kHz.
SEEK makes `WRITE == READ`, writes `ORIGIN_NS` as the seek media time minus
the cumulative `READ` time (the first new sample's ring position), increments
`GENERATION`, clears EOF, and
resynchronises if READ advances across the reset. The mixer responds to a
generation change by discarding its private FIFO.

The producer holds an exclusive advisory lock on the voice file for its whole
lifetime. On OPEN, an unlocked same-name regular file owned by the effective
user is a crashed producer's stale name and is atomically replaced; a locked
file is never taken over. Removing the file closes the voice. A producer must
never shorten a published mapping.

The current 60 ms mixer latency is nominal. The Pi 5 and desktop output paths
still need physical latency calibration and long-run A/V drift measurement;
no card or audio-device proof is claimed by this host tranche.
