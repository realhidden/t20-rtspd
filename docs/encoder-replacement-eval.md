# Evaluating an open-source replacement for the IMP H.264 encoder

**Status: investigated, not recommended. The premise doesn't hold on this SoC.**
Written 2026-09-28 while profiling `the test unit`.

## The question

Can we replace the closed `libimp` / `i264e` H.264 encoder with something
open, to cut CPU and bytes?

## Short answer

No, not on the T20. The H.264 encoder is a **hardware block in the SoC**, so
there is no open-source implementation that could replace it — the silicon
does the encoding. The only open alternatives are *software* encoders
(x264/x265), which are two to three orders of magnitude more expensive than
the hardware path we already use. Every measurement below points the same way.

## Where the CPU actually is

From `tools/profile-cam.sh` on `the test unit` (45s window, steady state):

| Thread | CPU | |
|---|---|---|
| `Encoder-0` (IMP H.264) | **24.18%** | vendor driver + hardware block |
| `t20rtspd` main loop | 0.13% | ours |
| `t20rtspd` uploader | 0.02% idle / ~3.3% mid-upload | ours |
| Framesource / FS-tick / ISP | 0.29% | vendor |

Our own code is ~0.15% of the process idle. **There is no software
optimisation available that matters** — the 24% is not in code we can edit.

## The hardware is real, and it is good

Ingenic's T20 product brief describes the "Hilex Video Processing Engine"
(VPU) as "a combination of hardware and a micro code engine", consuming
**<50 mW while encoding H.264 1080p@40fps** at High Profile, and rated to
encode 1080p@30fps while simultaneously running video analytics and Linux
applications on the CPU. The VPU is clocked at 333 MHz.

We are asking it for **720p@3fps**. The 24% we measure is the driver overhead
around an encoder that is barely being used.

## Why software encoding is not the answer

The T20 is a **1.0 GHz single-core XBurst (MIPS32r2)** with 128 KB L2 cache and
a 128 KB cache budget per core. For scale, HandBrake benchmarks x264 at
~70 fps of 1080p on a modern desktop core. A 1 GHz MIPS core is roughly two
orders of magnitude slower for this workload. Realistic expectation for
x264 on XBurst at 720p: **well under 1 fps, saturating the single core.**

Swapping a 24% hardware path for a >100% software path is a ~100x regression,
and it would also make the wifi/airtime problem far worse, not better.

## The T20 has no H.265, and the SDK does not offer one

This is the more interesting constraint. Hardware H.265 is the thing that
would actually cut bytes (typically 30-50% at equal quality for the same
hardware cost), and the T20 cannot do it:

- Our vendored `include/imp_sys/imp/imp_common.h` defines only
  `PT_JPEG` and `PT_H264` in `IMPPayloadType`. There is no H.265.
- The T20 product brief lists "Up to H264 1080P@40fps encode" and no H.265.
- Cross-referenced capability tables for the Ingenic line confirm:

  | SoC | H.265 in hardware |
  |---|---|
  | T20, T21, T23 | No |
  | T30, T32, T33, T40, T41 | Yes (Radix / Hera VPU blocks) |

  (Beware: en.ingenic.com.cn's product page for "H.265/H.264/JPEG" is a
  *different* part, not the T20.)

Note also that "Smart H.264" — Ingenic's claimed 20% bitrate saving — is
**already enabled** here via `ENCODING_TYPE = 3` (`ENC_RC_MODE_SMART`).

## What *is* open, and what it would buy

| Thing | Status | Usefulness |
|---|---|---|
| `libimp.so` | closed binary; **headers and API docs are open** (`imp`, ISVP SDK) | what we already compile against |
| `soc_vpu` / `jz_nvpu` kernel driver | closed in the camera firmware, but **open GPL source exists** in `thingino-linux` (`drivers/video/soc_vpu/`, incl. `helix.c` register programming) | lets you read/tune the driver, e.g. `/proc/jz/helix/param`, IVDC overflow handling. Same hardware, so **no CPU/bytes win** |
| MXU2 SIMD | `gtxaspec/ingenic-mxu` — shim headers + GCC/binutils patches, 128-bit SIMD on T20 | would accelerate *our* code, which is 0.15% of CPU. Nothing to gain |
| `raptor-hal` | open HAL abstracting T20→T41 and all three SDK generations, H.264/H.265/JPEG | genuinely useful **if we migrate SoCs** |

So there is real open-source work around the encoder, but it is all about
*controlling and porting* the hardware, not replacing it.

## Conclusion and the actual levers

The encoder is silicon; the only ways to reduce bytes are to change what we ask
it for. In descending order of value, all of which are config-level:

1. **Fix the IR-cut / night mode.** Night mode has never triggered
   (`Switching to NIGHT` count = 0) and the IR LEDs have never turned on,
   because the thresholds (`NIGHT=8,000,000`, `DAY=2,000,000`,
   `IR_LED=10,000,000`) sit far above the observed EV range. The camera is
   encoding a dark, un-IR'd, noise-dominated image 24/7. Noise is expensive to
   encode *and* defeats the bitrate cap. This is the biggest remaining win and
   it needs one night-time EV reading to calibrate.
2. **Lower the bitrate cap** — done: `MAX_BITRATE` 3000→1000 and
   `QUALITY_LVL` 4→5, which took 1263 → 420 kbps.
3. **Lower output fps** — done: `RATENUM` 5→3, which took 420 → 230 kbps at
   identical CPU. Free, because encoder CPU tracks the *image*, not the
   encoded frame count (verified: 5fps vs 3fps = 25.52% vs 24.16%).
4. **Do not lower the sensor fps** — measured actively harmful. At 3 sensor
   fps the AE cranks aGain to 37 dB, and the noisy result pushed encoder CPU
   to 60.6% *and* overran the bitrate cap to 740 kbps.

If a hardware change is ever on the table, a **T33/T40-based camera with
hardware H.265** is the only move that materially changes the encoding
economics, and `raptor-hal` is the ready-made path for reusing this codebase
across the SDK generations.

## Sources

- Ingenic T20 product brief (Hilex VPU, power, encode capability):
  `https://znanev.github.io/files/T20_PB.PDF`
- Ingenic encoder-block capability table (which SoC has H.265, VPU clocks):
  `https://gist.github.com/gtxaspec/86213e95796c912461a8566b817a6193`
- `raptor-docs` encoder API + per-SoC H.265 support matrix:
  `https://github.com/gtxaspec/raptor-docs/blob/main/03-sdk-encoder.md`
- Open `soc_vpu`/`helix` kernel driver:
  `https://github.com/gtxaspec/thingino-linux`
- MXU2/MXU3 SIMD shims and toolchain patches:
  `https://github.com/gtxaspec/ingenic-mxu`
- ISVP SDK overview (libimp is a closed library, docs/API open):
  `https://jmichault.github.io/ipcam-100-dok/`
- XBurst core/SoC history: `https://en.wikipedia.org/wiki/XBurst`
- HandBrake encoder throughput reference: `https://handbrake.fr/docs/en/1.9.0/technical/performance.html`
