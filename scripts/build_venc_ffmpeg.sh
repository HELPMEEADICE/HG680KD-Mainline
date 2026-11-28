#!/usr/bin/env bash
set -euo pipefail

root=${1:?remote workspace required}
work=${2:?FFmpeg build directory required}
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
cc="$root/out/musl-arm64/bin/musl-gcc"
cflags="-mno-outline-atomics -idirafter $root/out/headers-arm64/usr/include"
export PATH="${cross%/*}:$PATH"
export LC_ALL=C

cd "$work"
echo '7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3  ffmpeg-8.1.3.tar.xz' | sha256sum -c -
tar -xf ffmpeg-8.1.3.tar.xz
mkdir -p build
cd build
"$work/ffmpeg-8.1.3/configure" --arch=aarch64 --target-os=linux --enable-cross-compile \
    --cross-prefix="$cross" --cc="$cc" --disable-autodetect --disable-doc --disable-debug \
    --disable-ffplay --disable-ffprobe --disable-everything --disable-avdevice \
    --extra-cflags="$cflags" --extra-ldflags=-static --enable-v4l2-m2m \
    --enable-encoder=h264_v4l2m2m --enable-decoder=rawvideo,h264 --enable-parser=h264 \
    --enable-demuxer=rawvideo,h264,mov --enable-muxer=h264,mp4 --enable-protocol=file,pipe \
    --enable-filter=scale,format --enable-bsf=h264_mp4toannexb
make -j"${JOBS:-$(nproc)}" ffmpeg
file ffmpeg
sha256sum ffmpeg
"$cc" -static -O2 -Wall -Wextra -Werror -mno-outline-atomics \
    -idirafter "$root/out/headers-arm64/usr/include" "$work/venc-ioctl-smoke.c" -o venc-ioctl-smoke
sha256sum venc-ioctl-smoke
