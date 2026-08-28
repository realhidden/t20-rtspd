#!/bin/bash
# Build t20-rtspd using Docker with the MIPS cross-compilation toolchain.
# Usage: ./build.sh
#
# The MIPS toolchain is x86_64 only, so we force --platform linux/amd64.
# On Apple Silicon this runs under QEMU emulation (slower but works).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Named volume caches the toolchain and cross-built ffmpeg/mbedtls in /build
# so repeated builds skip the ~15 min of QEMU-emulated dependency building.
docker run --rm --platform linux/amd64 \
    -v "$SCRIPT_DIR:/root/" \
    -v t20rtspd-build-cache:/build \
    debian:bookworm \
    bash /root/build_docker.sh
