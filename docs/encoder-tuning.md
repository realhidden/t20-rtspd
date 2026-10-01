# Encoder tuning on T20 / JXF23

Measured on a test unit (T20/JXF23 1920x1080) with `tools/profile-cam.sh` for CPU and
actual chunk sizes from the uploader log for bytes. All figures are steady
state, daytime, 3 fps, excluding the first chunk after a restart while the AE
settles.

## The one thing to know first

`MAX_BITRATE` is a **byte** dial, not a CPU dial. Between 380 and 600 at
1080p the encoder's CPU moved 3.38% → 3.42% — 0.04 points for a 53% change
in bitrate. So compression can be tuned freely for storage and WiFi airtime
without touching the CPU budget.

Resolution *is* a CPU dial, but after the scaler fix both options are cheap:

| Resolution | Encoder CPU |
|---|---|
| 1280x720 | 1.9% |
| 1920x1080 (sensor native) | 3.4% |

## The measured curve

| Config | Actual bitrate | Volume | Encoder CPU |
|---|---|---|---|
| 1080p, cap 380 | 404 kbps | **4.06 GiB/day** | 3.38% |
| 1080p, cap 600 | 620 kbps | 6.2 GiB/day | 3.42% |
| 720p, cap 880 | 432 kbps | 4.2 GiB/day | 1.9% |

Night sits below all of these: 2 fps with `[night] BITRATE = 300` (300 kbps
at 1080p) and ISP denoise engaged, so a full day lands a little under the
daytime figure.

**Recommended default: 1920x1080, 3 fps, `MAX_BITRATE = 380`** — native
sensor resolution at roughly the byte volume the 720p configuration used to
cost, for 3.4% CPU.

## Where this came from

Before the framesource scaler fix, the same daemon measured 24.18% encoder
CPU and produced chunks around 17.1 MB. Two things changed:

1. **The scaler fix removed a software downscale.** Channel 0 had
   `.scaler.enable = 0`, so libimp was scaling 1920x1080 → 1280x720 in
   userspace. That single mistake was ~22% of a core. See the commit on
   `sample_framesource_set_output_size()`.
2. **The bitrate cap was doing nothing useful.** `[user] BITRATE` is unused
   in SMART mode; the effective ceiling came from `[smart] MAX_BITRATE`
   scaled against a 1920x1080 reference, and the encoder was running at
   92% of that ceiling because the content was complex enough to saturate it.

## Gotchas worth knowing

- **The RC overruns the cap on complex content.** Once QP is pinned at
  `MAX_QP` the bitrate clamp stops biting (`imp_encoder.h`: "when the image
  QP reaches maxQp, QP is clamped and the bitrate clamp loses effect"). Real
  output runs a little over the nominal cap. Raising `MAX_QP` trades
  quality for the ability to hit a lower cap.
- **Cap values above ~1000 stop helping on a T20** and only inflate output.
- **The ISP denoise raises contrast**, so the encoder sees more detail and
  will use more of its bitrate budget at the same cap. Turning it off
  (`[night] ISP_DENOISE = 0`) reduces night bytes at the cost of a noisier
  picture.
- **Night is the noisy part of the day.** This unit has no working IR
  illuminator (no `/dev/pwm` in this firmware, and no PWM channel or GPIO
  moves the sensor), so the AE sits at 103-128 dB of analog gain. That noise
  is what pushes the night bitrate over its cap and it is why the ISP
  denoise exists. The test unit reports this as a hardware fault via
  `/var/run/hwstatus`.

## What did not work, so it is not worth re-trying

- Lowering the **sensor** frame rate: at 3 fps the AE compensates with 37 dB
  of analog gain and encoder CPU rose to 60.6% with output overrunning to
  740 kbps. Both metrics got worse.
- **Encoder-side knobs** — `SetChnColor2Grey`, `SetChnDenoise`,
  `SetChnHSkipBlackEnhance` — made no measurable difference to bytes or CPU
  while the bitrate cap was the binding constraint. They remain available as
  config options, defaulting off.
- **Rate control mode**: FIXQP (no RC, no scene change, no frame skip) cost
  23.04% against SMART's 24.18%, so RC is not where the time was.
- **Dropping resolution to cut CPU** was bad advice while the scaler bug was
  live. The apparent saving was the downscale, not the pixels.
