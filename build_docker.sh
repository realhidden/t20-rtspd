#!/bin/bash
set -e

echo "=== Step 1: Setting up cross-compilation toolchain ==="
mkdir -p /build && cd /build
apt-get update -o Acquire::AllowInsecureRepositories=true
apt-get install -y --allow-unauthenticated libarchive-tools wget git build-essential libcurl4-openssl-dev libssl-dev bzip2

if [ ! -d /build/mips-gcc472-glibc216-64bit ]; then
    echo "Downloading MIPS toolchain..."
    wget -q https://github.com/Dafang-Hacks/Ingenic-T10_20/raw/master/resource/toolchain/mips-gcc472-glibc216-64bit-r2.3.3.7z
    # bsdtar (libarchive) instead of p7zip: newer p7zip refuses the toolchain's
    # relative .so symlinks ("Dangerous link path was ignored") and aborts.
    bsdtar xf mips-gcc472-glibc216-64bit-r2.3.3.7z
fi
export PATH=/build/mips-gcc472-glibc216-64bit/bin/:$PATH

echo "=== Step 2: Cross-compiling minimal FFmpeg ==="
FFMPEG_VERSION=4.4.8
FFMPEG_SHA256=c73848c4ae283d9eaee7be3b276affbc3543380483555500d0dd2c9b7e1c39c3
if [ ! -f /build/ffmpeg-mips-install/lib/libavformat.a ]; then
    cd /build
    if [ ! -d /build/ffmpeg-$FFMPEG_VERSION ]; then
        wget -q https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
        echo "$FFMPEG_SHA256  ffmpeg-$FFMPEG_VERSION.tar.xz" | sha256sum -c -
        tar xf ffmpeg-$FFMPEG_VERSION.tar.xz
    fi
    cd /build/ffmpeg-$FFMPEG_VERSION
    ./configure \
        --cross-prefix=mips-linux-uclibc-gnu- --arch=mips --target-os=linux \
        --enable-cross-compile --enable-static --disable-shared --disable-programs --disable-doc \
        --disable-avdevice --disable-swresample --disable-swscale --disable-postproc --disable-avfilter \
        --disable-network --disable-encoders --disable-decoders --disable-hwaccels \
        --disable-muxers --enable-muxer=matroska \
        --disable-demuxers --disable-parsers --enable-parser=h264 \
        --disable-bsfs --enable-bsf=extract_extradata \
        --disable-protocols --enable-protocol=file \
        --disable-indevs --disable-outdevs --disable-filters --disable-debug \
        --disable-asm --disable-zlib --disable-runtime-cpudetect \
        --extra-cflags="-O2 -march=mips32r2" --prefix=/build/ffmpeg-mips-install
    make -j$(nproc)
    make install
fi

echo "=== Step 2b: Cross-compiling mbedtls ==="
MBEDTLS_VERSION=2.28.10
MBEDTLS_SHA256=19e5b81fdac0fe22009b9e2bdcd52d7dcafbf62bc67fc59cf0a76b5b5540d149
if [ ! -f /build/mbedtls-mips-install/lib/libmbedtls.a ]; then
    cd /build
    if [ ! -d /build/mbedtls-$MBEDTLS_VERSION ]; then
        # 2.28.10 is the final 2.28 LTS release (fixes CVE-2025-27810/27809)
        wget -q https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2
        echo "$MBEDTLS_SHA256  mbedtls-$MBEDTLS_VERSION.tar.bz2" | sha256sum -c -
        tar xf mbedtls-$MBEDTLS_VERSION.tar.bz2
    fi
    cd /build/mbedtls-$MBEDTLS_VERSION
    make -j$(nproc) CC=mips-linux-uclibc-gnu-gcc AR=mips-linux-uclibc-gnu-ar \
        CFLAGS="-O2 -march=mips32r2 -std=gnu99" lib
    make CC=mips-linux-uclibc-gnu-gcc AR=mips-linux-uclibc-gnu-ar \
        CFLAGS="-O2 -march=mips32r2 -std=gnu99" DESTDIR=/build/mbedtls-mips-install install
fi

echo "=== Step 2c: Copying mbedtls libs and headers to project ==="
mkdir -p /root/lib/mbedtls
cp /build/mbedtls-mips-install/lib/libmbedtls.a /root/lib/mbedtls/
cp /build/mbedtls-mips-install/lib/libmbedx509.a /root/lib/mbedtls/
cp /build/mbedtls-mips-install/lib/libmbedcrypto.a /root/lib/mbedtls/
cp -r /build/mbedtls-mips-install/include/mbedtls /root/include/

echo "=== Step 3: Copying FFmpeg libs and headers to project ==="
cd /root
cp /build/ffmpeg-mips-install/lib/libavformat.a lib/ffmpeg/
cp /build/ffmpeg-mips-install/lib/libavcodec.a lib/ffmpeg/
cp /build/ffmpeg-mips-install/lib/libavutil.a lib/ffmpeg/
cp -r /build/ffmpeg-mips-install/include/libavformat/* include/ffmpeg/libavformat/
cp -r /build/ffmpeg-mips-install/include/libavcodec/* include/ffmpeg/libavcodec/
cp -r /build/ffmpeg-mips-install/include/libavutil/* include/ffmpeg/libavutil/

echo "=== Step 4: Building t20-rtspd ==="
cd /root
make clean || true
make

echo "=== Build complete ==="
ls -la t20-rtspd
mips-linux-uclibc-gnu-size t20-rtspd
