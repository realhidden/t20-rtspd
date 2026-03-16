#!/bin/bash
set -e

echo "=== Step 1: Setting up cross-compilation toolchain ==="
mkdir -p /build && cd /build
apt-get update && apt-get install -y p7zip wget git build-essential libcurl4-openssl-dev libssl-dev bzip2

if [ ! -d /build/mips-gcc472-glibc216-64bit ]; then
    echo "Downloading MIPS toolchain..."
    wget -q https://github.com/Dafang-Hacks/Ingenic-T10_20/raw/master/resource/toolchain/mips-gcc472-glibc216-64bit-r2.3.3.7z
    p7zip -d mips-gcc472-glibc216-64bit-r2.3.3.7z
fi
export PATH=/build/mips-gcc472-glibc216-64bit/bin/:$PATH

echo "=== Step 2: Cross-compiling minimal FFmpeg ==="
if [ ! -f /build/ffmpeg-mips-install/lib/libavformat.a ]; then
    cd /build
    if [ ! -d /build/ffmpeg-4.4.6 ]; then
        wget -q https://ffmpeg.org/releases/ffmpeg-4.4.6.tar.xz
        tar xf ffmpeg-4.4.6.tar.xz
    fi
    cd /build/ffmpeg-4.4.6
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
if [ ! -f /build/mbedtls-mips-install/lib/libmbedtls.a ]; then
    cd /build
    if [ ! -d /build/mbedtls-2.28.8 ]; then
        cp /root/mbedtls-2.28.8.tar.bz2 /build/
        tar xf mbedtls-2.28.8.tar.bz2
    fi
    cd /build/mbedtls-2.28.8
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
