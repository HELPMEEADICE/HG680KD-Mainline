#!/usr/bin/env bash
set -euo pipefail

root=${1:-"$HOME/hg680kd-mainline"}
work="$root/out/vdec-ffmpeg"
src="$root/source/ffmpeg-v4l2request"
revision=b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935
deps="$work/alpine"
repo=https://mirror.nju.edu.cn/alpine/v3.22/main/aarch64
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
cc="$root/out/musl-arm64/bin/musl-gcc"
headers="$root/out/headers-arm64/usr/include"

mkdir -p "$deps" "$work/ffmpeg-build" "$work/serve"
if [[ ! -d $src/.git ]]; then
    git clone --single-branch --depth 1 --branch v4l2-request-n8.1 \
        https://github.com/Kwiboo/FFmpeg.git "$src"
fi
test "$(git -C "$src" rev-parse HEAD)" = "$revision"
patch="$root/scripts/ffmpeg-v4l2request-nv21.patch"
if ! git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$src" apply "$patch"
fi

for package in musl-1.2.5-r13 libdrm-2.4.124-r0 libdrm-dev-2.4.124-r0 \
               eudev-libs-3.2.14-r5 eudev-dev-3.2.14-r5; do
    curl -fsSL --retry 2 --max-time 30 "$repo/$package.apk" -o "$deps/$package.apk"
    tar --warning=no-unknown-keyword -xzf "$deps/$package.apk" -C "$deps"
done
sha256sum "$deps"/*.apk
ln -sfn ld-musl-aarch64.so.1 "$deps/lib/libc.so"

cd "$work/ffmpeg-build"
export PKG_CONFIG_LIBDIR="$deps/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$deps"
"$src/configure" --arch=aarch64 --target-os=linux --enable-cross-compile \
    --cross-prefix="$cross" --cc="$cc" --disable-autodetect \
    --pkg-config=pkg-config \
    --extra-cflags="-mno-outline-atomics -idirafter $headers" \
    --extra-ldflags="-L$deps/lib -Wl,-rpath-link,$deps/lib:$deps/usr/lib" \
    --extra-libs="-l:libc.musl-aarch64.so.1" \
    --disable-doc --disable-debug --disable-ffplay --disable-ffprobe \
    --disable-everything --disable-avdevice \
    --enable-libdrm --enable-v4l2-request \
    --enable-hwaccel=h264_v4l2request,hevc_v4l2request \
    --enable-decoder=h264,hevc --enable-parser=h264,hevc \
    --enable-demuxer=mov,h264,hevc --enable-protocol=file,pipe \
    --enable-encoder=wrapped_avframe,rawvideo --enable-muxer=null,framemd5 \
    --enable-filter=hwdownload,format,scale \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
make -j"${JOBS:-$(nproc)}" ffmpeg
out="$work/serve"
install -m755 ffmpeg "$out/ffmpeg"
install -m755 "$deps/lib/ld-musl-aarch64.so.1" "$out/ld-musl-aarch64.so.1"
install -m755 "$deps/usr/lib/libdrm.so.2" "$out/libdrm.so.2"
install -m755 "$deps/usr/lib/libudev.so.1" "$out/libudev.so.1"
"${cross}readelf" -h -l -d "$out/ffmpeg"
for lib in "$out"/lib*.so.*; do
    "${cross}readelf" -d "$lib" | grep NEEDED
done
