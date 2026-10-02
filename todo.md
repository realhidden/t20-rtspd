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

**RESOLVED: not a bug.** The log shows frames climbing steadily to 110,400
with `poll_timeouts` frozen at 26321 -- the counter had simply stopped
incrementing because no further timeouts were occurring. `frames=900` was a
freshly restarted daemon three minutes in, not a stall. The counter only
advances on frame receipt, so a run with no timeouts prints an unchanging
`poll_timeouts`, which reads exactly like a hang if you do not check
`frames` at the same time.

Lesson: always read the two counters together. `frames` is the liveness
signal; `poll_timeouts` only tells you about polling.

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

**RESOLVED: the claim was wrong, saving is real but tiny.**

Measured 60s windows at 1920x1080 with 5fps output, sensor 10 vs sensor 5:

| thread | sensor 10 | sensor 5 |
|---|---|---|
| Encoder-0 | 4.97% | 4.80% (4.75, 4.85) |
| main | 0.27% | 0.22% |
| Framesource-0 | 0.18% | 0.18% (0.08 repeat) |
| FS(0)-tick | 0.05% | 0.03% |
| **total** | **5.52%** | **5.24%** |

About **0.2-0.3pp, 4-5% of total**. The old log line claimed skipped frames
"are still produced and cost CPU to scale" -- wrong at native size. With the
encode size equal to the sensor size there is no scaler in the path, so a
frame the encoder never sees costs almost nothing. `Framesource-0`'s context
switches halve (606 -> 306), which confirms the sensor really does stop
producing the skipped frames; there is just no work in them to save.

Video verified at sensor 5: 1920x1080, avg_frame_rate 5/1, 1088 packets,
strictly increasing DTS.

Decision: **keep SENSOR_FPS_NUM matched to RATENUM.** It is free, and at a
non-native encode size the skipped frames would cost real work. The log
message is corrected to say so instead of overstating the cost.

Note on low light: halving the sensor rate doubles integration time per
frame, which trades motion blur for sensitivity. Not measurable here -- the
test unit has no working illuminator, so there is no dark scene to compare
against. Re-check if the LED array is ever repaired.

Status: DONE

## 3. Re-profile CPU to find the new top consumer

**RESOLVED by the item 2 measurement above.** The ranking at 1080p/5fps:

| thread | CPU | role |
|---|---|---|
| Encoder-0 | 4.8-5.0% | H.264 encode |
| main capture loop | 0.22-0.27% | GetStream + MKV write |
| Framesource-0 | 0.08-0.18% | frame delivery |
| FS(0)-tick | 0.03-0.05% | frame source tick |
| isp_tuning_deam | 0.03% | AE convergence |
| ENC(0)-update_f | 0.02-0.03% | encoder rate control |
| everything else | ~0% | uploader idle, HTTP idle |

**Encoder-0 is still ~90% of all CPU the process uses.** Nothing else is
worth optimising: the whole non-encoder side is under 0.6%. The original
24.18% hotspot is gone and the next-biggest thread is two orders of magnitude
smaller.

Consequence for the rest of this list: the only remaining lever on CPU is the
encoder itself, via resolution, fps, or bitrate/QP -- not any of the
support threads. `Encoder-0` is 4.93% usr / 0.03% sys, so it is genuinely
doing the encode work in userspace, not spinning on syscalls.

Status: DONE

## 4. Reduce upload burstiness

Upload cap is `RATE_LIMIT_KBPS=300` (300KB/s) but 5fps generates ~78KB/s
average. Adaptive probe has been measuring ~980-1056KB/s available, so the
cap is doing the limiting.

Drop the cap to ~100-150KB/s: smoother uploads, less WiFi radio time, and
less burst pressure on the SD card and WiFi. Uploads arrive as 12 x 1MB
chunks, which is the bursty part.

**RESOLVED: cap lowered 300 -> 150 KB/s, verified in production.**

Generation at 5fps is only ~50KB/s (404kbps CBR), so the old 300KB/s cap was
~6x the rate at which data is produced -- uploads burst at the cap then idle.
Measured wifi headroom is 940-1108KB/s, so 150KB/s is still ~3x generation
and the uploader keeps up with margin.

Verified after the change: adaptive probe logs `rate=150KB/s`, uploads
succeed, 4 chunks uploaded with 0 failures, pending MKVs steady at 2 (one
open, one in flight) rather than growing. A transient count of 3 right after
the restart was rotation latency, not a backlog -- it settled on its own.

Side note: `exampleconf.ini` documents `TOKEN = your-shared-secret-token` as
a placeholder, but the live config carries a real UUID. Not in the repo, so
not exposed, but it is a live credential sitting in a world-readable file on
the card.

Status: DONE

## 5. Tune GOP / chunk duration

`Smart GOP: 6s (maxGop will scale with fps)`, chunk 300s.

- Longer GOP: lower bitrate and upload overhead.
- Shorter GOP: better seeking, and finer restart granularity if an upload
  fails partway.

At 5fps a 6s GOP is 30 frames. Worth an A/B on bitrate at fixed quality.

**RESOLVED, and it turned up an SDK quirk worth knowing.**

Measured keyframe spacing does not match `GOP_SEC` at all:

| `GOP_SEC` | configured `maxGop` | measured spacing | keyframe bytes |
|---|---|---|---|
| 6 | 30 frames | **36.0s** (180 frames) | 5.5% |
| 2 | 10 frames | **12.0s** (60 frames) | 14.6% |

`imp-common.c` computes `maxGop = GOP_SEC * frmRateNum / frmRateDen`, so 6s
should give 30 frames = 6.0s at 5fps. Instead the encoder emits a keyframe
every **6x** that many frames. Both test points are exactly 6x, so the
multiplier is consistent, not noise.

**Cause: the SDK, not the night toggle.** My first hypothesis was
`apply_night_encoding()` -- it calls `IMP_Encoder_SetChnFrmRate()` on every
day/night transition to switch 5fps <-> 2fps, and `maxGop` is only ever set
once at channel creation (`imp-common.c:787`). But the test unit had 2 night
transitions before the first measurement, so I re-measured after a restart
with **zero** rate changes since: still 36.0s intervals, keyframes at
0/36/72/108/144/180s. The 6x multiplier is therefore inherent to how this SDK
applies `maxGop` and is unrelated to night switching.

`imp_encoder.h:214` documents `maxGop` as "must be a multiple of the frame
rate", so the SDK is rescaling it against its own internal rate rather than
honouring the value passed in. There is no API to set it directly -- it is
only reachable through the full `IMPEncoderCHNAttr`.

Practical effect: `GOP_SEC` is a *relative* knob here, not an absolute one.
Divide the intended seconds by 6 to get what you actually want.

Decision: **keep GOP_SEC = 6** (restored). The longer spacing is *cheaper* --
keyframes are only 5.5% of the stream versus 14.6% at GOP_SEC=2 -- and a 36s
seek granularity is acceptable for security footage. Shortening it to gain
seek precision would cost roughly 9% more bytes for no CPU benefit.

Status: DONE (with a follow-up noted above)

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
