# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

T20-RTSPD is a lightweight RTSP server for the Ingenic T20/T20L SoC, targeting low-memory cameras like the Xiaomi Xiaofang 1S and WyzeCam v2. It captures H.264 video from the camera sensor and streams it via RTSP using the live555 library.

## Build Commands

**Requires the MIPS cross-compilation toolchain** (`mips-linux-uclibc-gnu-` prefix). Cannot be built natively on macOS/x86.

```bash
make          # Build the t20-rtspd binary (auto-generates version.h from git)
make clean    # Remove object files, binary, and version.h
```

Docker-based build (recommended):
```bash
docker run --rm -ti -v $(pwd):/root/ debian
# Inside container:
mkdir /build && cd /build
apt-get update && apt-get install -y p7zip wget git build-essential libcurl4-openssl-dev libssl-dev nlohmann-json3-dev
wget https://github.com/Dafang-Hacks/Ingenic-T10_20/raw/master/resource/toolchain/mips-gcc472-glibc216-64bit-r2.3.3.7z
p7zip -d mips-gcc472-glibc216-64bit-r2.3.3.7z
export PATH=/build/mips-gcc472-glibc216-64bit/bin/:$PATH
cd ~ && make
```

## Architecture

### Process Model (fork-based)

`on_demand_rtsp_server.cpp:main()` forks into two processes connected by a FIFO (`/tmp/h264_fifo`):

- **Parent process**: Capture loop — initializes the IMP SDK, encodes H.264 frames, and writes them to the FIFO
- **Child process**: RTSP server — live555 event loop serving the stream at `rtsp://<ip>:554/unicast`

### Source File Roles

| File | Role |
|------|------|
| `on_demand_rtsp_server.cpp` | Entry point, fork, RTSP server setup (live555) |
| `capture_and_encoding.cpp` | IMP SDK init, H.264 encoding loop, OSD rendering, night mode thread |
| `imp-common.c` | Low-level IMP SDK wrapper (ISP, framesource, encoder, OSD init/exit) |
| `pwm.c` | `/dev/pwm` kernel driver interface for IR LED control |
| `ini.c` | INI config file parser |

### IMP SDK Pipeline

Sensor → FrameSource → [OSD (optional)] → H.264 Encoder → FIFO → live555 RTSP

### Compile-Time Feature Flags

Defined/undefined in `capture_and_encoding.cpp`:
- `ENABLED_OSD` — on-screen timestamp overlay using bitmap fonts from `bgramapinfo.h`
- `NIGHTMODE_SWITCH` — automatic IR LED and IR-cut filter control via photosensitive detection thread
- `SUPPORT_RGB555LE` — alternative OSD color format (defined in `imp-common.h`)

### Sensor Selection

Set via `#define SENSOR_*` in `imp-common.h`. Default is `SENSOR_JXF23` (1920x1080). Each sensor define sets name, I2C address, resolution, and channel configuration.

### Runtime Configuration

`exampleconf.ini` — parsed at startup to configure encoding parameters:
- `ENCODING_TYPE`: 0=FixQP, 1=CBR, 2=VBR, 3=Smart
- `WIDTH`, `HEIGHT`: stream resolution
- `RATENUM`/`RATEDEN`: frame rate (FPS = RATENUM/RATEDEN)
- `BITRATE`, `MAXQP`, `MINQP`: rate control
- `PROFILE`: 0=Baseline, 1=Main, 2=High

### Undocumented SDK Calls

`capture_and_encoding.cpp` uses reverse-engineered calls `IMP_OSD_SetPoolSize()` and `IMP_Encoder_SetPoolSize()` to increase memory pools for 64MB T20L devices.

## Key Constraints

- Cross-compiled for MIPS32r2 with uclibc — all libraries in `lib/` are pre-compiled for this target
- Live555 headers in `include/live555/`, IMP SDK headers in `include/imp_sys/`
- C files compiled with gcc, C++ files with g++ (mixed C/C++ project, `extern "C"` blocks used)
- Output binary is stripped (`$(STRIP) -s`)
