t20-rtspd — camera daemon for Ingenic T20/T20L
==================

A single-process camera daemon for Xiaomi Dafang / Xiaofang 1S style devices
(Ingenic T20/T20L, MIPS32r2, uclibc). It captures H.264 from the IMP SDK and
does everything on-device:

- **MKV recording** — rotating chunks at IDR boundaries via a minimal static
  FFmpeg (matroska muxer only), with a disk-usage guard
- **HTTPS upload** — chunked, resumable-by-server upload of finished chunks to
  a companion server, with static or adaptive (~80% of measured bandwidth)
  rate limiting so cameras don't saturate the WiFi uplink
- **JPEG snapshot server** — tiny HTTP server (`GET /snapshot`, `GET /status`)
  for Home Assistant; announced over mDNS
- **Autonight** — built-in day/night detection from ISP exposure (EMA with
  hysteresis), IR-cut filter + IR LED control, and night encoding overrides
- **Grafana Cloud push** — periodic metrics via InfluxDB line protocol
- **Audio** — optional G.711A microphone track muxed into the MKV

RTSP streaming was removed in 2026 — nothing in the target deployment used it
(HA consumes snapshots; recordings go over HTTPS). It lives in git history if
ever needed. The mDNS advertisement still uses the `_rtsp._tcp` service type
with a `path=/snapshot` TXT record, which Home Assistant's zeroconf discovery
matches on.

## Building

Cross-compiled for MIPS with a 2012-era uclibc toolchain — build in Docker:

```bash
./build.sh        # runs build_docker.sh inside debian:bookworm (linux/amd64)
```

The build downloads the pinned toolchain, FFmpeg 4.4.8 and mbedtls 2.28.10
(sha256-verified) automatically. `make` alone also works if the toolchain and
vendored libs under `lib/` and `include/` are already in place.

Output: a stripped `t20-rtspd` binary with the git commit hash embedded
(`/tmp/version` on the device).

## Configuration

Reads `/system/sdcard/config/donekamera.conf`, falling back to
`/system/sdcard/config/test.ini`. See `exampleconf.ini` for all sections:
`[user]` (encoding), `[smart]`, `[night]`, `[autonight]`, `[recording]`,
`[upload]`, `[http]`, `[grafana]`, `[audio]`. Unknown sections/keys are
ignored, so a shared config file can carry extra sections for other daemons.

## Credits

Originally an RTSP server derived from
[carrier-rtsp-server](https://github.com/beihuijie/carrier-rtsp-server), then
geekman's [t20-rtspd](https://github.com/geekman/t20-rtspd) (T20L pool-size
hacks, PWM IR LED control), later extended with recording/upload/snapshot/
autonight. Newer changes are 3-clause BSD licensed where applicable
(Copyright 2019 Darell Tan; Copyright 2026 the t20-rtspd authors).
