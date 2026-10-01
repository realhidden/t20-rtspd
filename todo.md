# Finetune TODO

Findings and candidate improvements from the T20 camera work, in priority
order. Items are ordered by expected value, not by effort.

## 1. Investigate `frames=900 poll_timeouts=198` (possible bug)

Observed in `/system/sdcard/log/t20rtspd.log`:

```
[main] Stats: frames=900 poll_timeouts=198 getstream_errors=0
```

- Frames **stopped incrementing at 900** while poll timeouts climbed
  108 -> 198, over roughly a 300s reporting window.
- Recording continued normally through it: 324s chunks, correct sizes.
- Earlier in the same session the same counter read
  `frames=6300 poll_timeouts=0`.

So in that run a 300s window passed with zero frames counted and ~200
timeouts on whatever channel that stat watches, while chn 0 kept recording.

Possibilities:
- A stalled `IMP_FrameSource_GetFrame` on a secondary channel.
- A counter that does not mean what it looks like (e.g. counting only a
  channel that is intentionally idle, or reset on some path).

Worth ten minutes. If it is a real stall it is worth more than every other
item on this list. Check first: which channel the counter follows, and
whether `poll_timeouts` is per-channel or global.

Status: TODO

## 2. Resolve `SENSOR_FPS_NUM=10` while encoding 5 fps

The daemon's own log contradicts itself:

```
[config] Note: encoding 5.00 of 10.00 sensor fps; the skipped sensor frames
        are still produced and cost CPU to scale
[capture] encode size 1920x1080 matches sensor: no scaler needed
```

If no scaler is needed, what is being paid for on the skipped frames? Either
the note is wrong, or per-frame ISP work still happens regardless.

Test: drop `SENSOR_FPS_NUM` to 5 and diff Encoder-0 *and* the ISP/scaler
threads, not just the encoder.

Note: the earlier "reducing sensor fps does not help CPU" result was
measured while the framesource scaler bug was still dominant and the encoder
thread was at 24%. It deserves a re-test now that the hotspot has moved.

Status: TODO

## 3. Re-profile CPU to find the new top consumer

Encoder-0 is ~5.33% at 1080p/5fps, down from 24.18% before the scaler fix.
The old hotspot is gone, so whatever ranks below it has probably changed and
the ranking is currently unknown. One profile run before optimising anything
else. Use `tools/profile-cam.sh`.

Status: TODO

## 4. Reduce upload burstiness

Upload cap is `RATE_LIMIT_KBPS=300` (300KB/s) but 5fps generates ~78KB/s
average. Adaptive probe has been measuring ~980-1056KB/s available, so the
cap is doing the limiting.

Drop the cap to ~100-150KB/s: smoother uploads, less WiFi radio time, and
less burst pressure on the SD card and WiFi. Uploads arrive as 12 x 1MB
chunks, which is the bursty part.

Status: TODO

## 5. Tune GOP / chunk duration

`Smart GOP: 6s (maxGop will scale with fps)`, chunk 300s.

- Longer GOP: lower bitrate and upload overhead.
- Shorter GOP: better seeking, and finer restart granularity if an upload
  fails partway.

At 5fps a 6s GOP is 30 frames. Worth an A/B on bitrate at fixed quality.

Status: TODO

## 6. Night params: needs a camera with a working illuminator

Current night overrides: 2fps, `ISP_DENOISE=96`, 300kbps, max_qp 51.

**The test unit cannot validate any of this** -- its IR array does not illuminate, so
night chunks are now skipped by the dark-scene skip and never evaluated. A
new camera with a working illuminator becomes the reference for night
quality. Until then these values are untested in production.

Status: BLOCKED (needs a second reference unit)

## 7. Decide a policy for the test unit's dead IR illuminator

Current behaviour: logs `[hwfault] IR LED array not illuminating` and keeps
`ir_led` in `/var/run/hwstatus`, so telemetry and the dashboard report it
forever.

Options:
- Keep reporting. Honest and visible, but permanently noisy.
- Set `IR_LED_OFF=1` and stop driving the array. Quieter, but loses the
  diagnostic and hides the fault.

Status: TODO (product decision)

## 8. Audio is disabled and untested

`[config] Audio: enabled=0`, so the G.711A path in `mkv_recorder.c`
(`mkv_recorder_write_audio_frame`) is unvalidated in production. Enabling it
is a real feature to test, not a tweak: muxing, timestamps against the new
microsecond time base, and the `audio` stream's effect on the dark-skip
window logic all need checking.

Status: TODO (product decision)

## Already done this session

- Framesource channel 0 `.scaler.enable` fix (Encoder-0 24.18% -> ~3.4%).
- MKV microsecond time base, fixing non-monotonic DTS across rate changes.
- Night threshold recalibration (live config had values an order of
  magnitude too high, so night mode never fired).
- Stale `/var/run/hwstatus` cleared at autonight startup.
- Dead mDNS shell-out removed from `main.cpp`, `client.cpp`,
  `setup-camera.sh`.
- 5 fps default, native 1920x1080.
- Dark-scene chunk skip (`94e5c20`).
- `tools/discover-cams.sh` network camera discovery (`bbc323b`).

## Known external blockers

- The second camera unit is absent at layer 2; cannot validate or deploy.
- the test unit's IR illuminator does not illuminate: no PWM channel or GPIO drives
  the array. Hardware fault, not software.
- `t20` GitLab push blocked by missing authorised SSH key; commits are local.
