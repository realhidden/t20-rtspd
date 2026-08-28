# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

t20-rtspd is a single-process camera daemon for Ingenic T20/T20L devices
(Xiaomi Dafang / Xiaofang 1S, MIPS32r2 + uclibc, 64 MB RAM class). It captures
H.264 via the IMP SDK, records rotating MKV chunks, uploads them over chunked
HTTPS, serves JPEG snapshots over HTTP, and runs automatic night-mode
switching. RTSP/live555 support was removed in 2026 — do not reintroduce the
live555 fork/FIFO architecture.

## Build Commands

**Requires the MIPS cross-compilation toolchain** (`mips-linux-uclibc-gnu-`
prefix). Cannot be built natively on macOS/x86.

```bash
./build.sh      # Docker build (debian:bookworm, linux/amd64): toolchain + ffmpeg + mbedtls + make
make            # Only if toolchain and vendored libs/ + include/ are present
make clean      # Remove object files, binary, and version.h
```

`version.h` is generated from the git commit hash at build time.

## Architecture

Single process, several threads:

| Thread | File | Role |
|--------|------|------|
| main | `main.cpp` | Entry point, config, init order, H264 capture loop → `mkv_recorder_write_frame` |
| autonight | `imp-common.c` (`sample_soft_photosensitive_thread`) | ISP exposure EMA → day/night switch, IR-cut + IR LED GPIO, `apply_night_encoding()` |
| MKV rotation | `mkv_recorder.c` | Chunk rotation at IDR boundaries, SPS/PPS caching, disk threshold guard |
| audio | `audio_capture.cpp` | IMP audio in → G.711A → `mkv_recorder_write_audio_frame` |
| upload | `file_uploader.c` | Scans output dir, uploads 1 MB chunks via `https_post_chunk`, adaptive/static pacing |
| snapshot HTTP | `http_server.c` | `GET /snapshot` (JPEG via `sample_do_get_jpeg_snap`), `GET /status` |
| grafana | `grafana_push.c` | Periodic metrics push (InfluxDB line protocol) |

Support files: `imp-common.c` (IMP SDK init/teardown, encoder setup, INI
config parser), `ini.c` (inih), `https_client.c` (mbedtls HTTPS client,
`adaptive_rate_t`).

## IMP SDK Pipeline

Sensor → FrameSource (chn 0/1) → H.264 Encoder (CBR/VBR/Smart/FixQP)
→ main loop → MKV recorder (+ optional audio muxing) and JPEG encoder (chn 2,
snapshots on demand).

Undocumented SDK call `IMP_Encoder_SetPoolSize()` raises the encoder pool for
64 MB T20L devices.

## Runtime Configuration

`/system/sdcard/config/donekamera.conf`, fallback `/system/sdcard/config/test.ini`.
Parsed by the handler in `imp-common.c` (`app_config_parse`), defaults set in
the same function. Unknown keys return 1 (ignored) — other daemons share the
file (`[telemetry]` belongs to the external client daemon). Camera name comes
from `/system/sdcard/config/cameraname` (legacy: `/system/sdcard/cameraname`)
and is sanitized to `[A-Za-z0-9_-]` because it is interpolated into a
`system()` mDNS command — keep that sanitization.

## Key Constraints

- Cross-compiled with gcc 4.7.2 / uclibc — no modern C/C++ features; C99/gnu99
- Vendored prebuilt libs in `lib/` (ffmpeg static, mbedtls static, IMP uclibc
  .so); headers in `include/`; ffmpeg and mbedtls are rebuilt from pinned
  sources by `build_docker.sh` (sha256-verified downloads)
- Do not link `libaudioProcess.so` — IMP_AI_*/IMP_AENC_* symbols come from
  libimp.so (see note in Makefile)
- Output binary is stripped (`$(STRIP) -s`)
- Sensor selected via `#define SENSOR_*` in `imp-common.h` (default JXF23
  1920x1080)
