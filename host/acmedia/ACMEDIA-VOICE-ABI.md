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

The runtime uses that latency-corrected value as the audio master. It admits
video up to 40 ms ahead, waits beyond that bound, and drops a video frame over
100 ms late. Those are queue bounds, not a physical-latency claim.

PLAY clears `PAUSED`; PAUSE sets it. `GAIN_Q16` is CONTROL VOLUME. SEEK first
makes `WRITE == READ`, writes the new `ORIGIN_NS`, increments `GENERATION`,
and clears EOF. The mixer responds to a generation change by discarding its
private FIFO. Removing the file closes the voice.

The current 60 ms mixer latency is nominal. The Pi 5 and desktop output paths
still need physical latency calibration and long-run A/V drift measurement;
no card or audio-device proof is claimed by this host tranche.
