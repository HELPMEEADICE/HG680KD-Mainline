#!/usr/bin/env bash
set -euo pipefail

root=${1:?remote workspace required}
work="$root/out/arm64-7.3.6-mpv-drm"
stage=${2:-all}
case "$stage" in
    all|sdk|ffmpeg|mpv) ;;
    *) printf 'unknown build stage: %s\n' "$stage" >&2; exit 2 ;;
esac
sdk="$work/sdk"
ffmpeg="$work/ffmpeg"
mpv="$work/mpv"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
cc="$root/out/musl-arm64/bin/musl-gcc"
headers="$root/out/headers-arm64/usr/include"
out="$work/serve"
jobs=${JOBS:-32}

mkdir -p "$work" "$out"
if [[ $stage == all || $stage == sdk ]]; then
    mkdir -p "$sdk"
    docker run --rm --platform linux/arm64 -v "$sdk:/target" hg680kd-lima-alpine:3.23 \
        sh -ec 'apk --root /target --initdb --no-scripts --keys-dir /etc/apk/keys \
            --repositories-file /etc/apk/repositories add \
            musl-dev linux-headers libdrm-dev eudev-dev mesa-dev mesa-dri-gallium \
            mesa-gbm mesa-egl mesa-gles libdisplay-info-dev libplacebo-dev \
            libass-dev alsa-lib-dev; apk --root /target info -v' | tee "$work/apk-build.log"
fi

if [[ $stage == all || $stage == ffmpeg ]]; then
    deps="$root/out/vdec-ffmpeg/alpine"
    if [[ ! -d $ffmpeg/.git ]]; then
        git clone --no-local "$root/source/ffmpeg-v4l2request" "$ffmpeg"
    fi
    test "$(git -C "$ffmpeg" rev-parse HEAD)" = b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935
    nv21="$root/scripts/ffmpeg-v4l2request-nv21.patch"
    tiled="$root/scripts/ffmpeg-v4l2request-mv320-tiled.patch"
    if ! git -C "$ffmpeg" apply --reverse --check "$tiled" 2>/dev/null; then
        git -C "$ffmpeg" apply --reverse --check "$nv21" 2>/dev/null || git -C "$ffmpeg" apply "$nv21"
        git -C "$ffmpeg" apply "$tiled"
    fi
    mkdir -p "$work/ffmpeg-build" "$work/ffmpeg-install"
    (
        cd "$work/ffmpeg-build"
        export PKG_CONFIG_LIBDIR="$deps/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$deps"
        "$ffmpeg/configure" --prefix=/usr --arch=aarch64 --target-os=linux \
            --enable-cross-compile --cross-prefix="$cross" --cc="$cc" --disable-autodetect \
            --pkg-config=pkg-config \
            --disable-static --enable-shared --disable-doc --disable-debug --disable-ffplay \
            --disable-everything --disable-avdevice --enable-libdrm --enable-v4l2-request \
            --extra-cflags="-mno-outline-atomics -idirafter $headers" \
            --extra-ldflags="-L$deps/lib -Wl,-rpath-link,$deps/lib:$deps/usr/lib" \
            --extra-libs="-l:libc.musl-aarch64.so.1" \
            --enable-hwaccel=h264_v4l2request,hevc_v4l2request,mpeg2_v4l2request \
            --enable-decoder=h264,hevc,mpeg2video,aac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le \
            --enable-parser=h264,hevc,mpegvideo,aac \
            --enable-demuxer=mov,matroska,mpegvideo,mpegts,h264,hevc,wav --enable-protocol=file,pipe \
            --enable-encoder=wrapped_avframe,rawvideo,pcm_s16le \
            --enable-muxer=null,framemd5,wav \
            --enable-filter=hwdownload,format,scale,aformat,aresample,atempo \
            --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
        make -j"$jobs"
        make DESTDIR="$work/ffmpeg-install" install
    ) >"$work/ffmpeg-build.log" 2>&1
fi

if [[ $stage == all || $stage == mpv ]]; then
    if [[ ! -d $mpv/.git ]]; then
        git clone --depth 1 --branch v0.41.0 https://github.com/mpv-player/mpv.git "$mpv"
    fi
    test "$(git -C "$mpv" rev-parse HEAD)" = 41f6a645068483470267271e1d09966ca3b9f413
    patch="$root/scripts/mpv-v4l2request-drmprime-overlay.patch"
    git -C "$mpv" apply --reverse --check "$patch" 2>/dev/null || git -C "$mpv" apply "$patch"
    ff="$work/ffmpeg-install/usr"
    docker run --rm --platform linux/arm64 --network none \
        -v "$sdk:/target" -v "$ff:/ff:ro" hg680kd-lima-alpine:3.23 \
        cp -a /ff/. /target/usr/
    cat >"$work/aarch64.ini" <<EOF
[binaries]
c = '$cc'
ar = '${cross}ar'
strip = '${cross}strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
sys_root = '$sdk'
pkg_config_libdir = '$sdk/usr/lib/pkgconfig'

[built-in options]
c_args = ['-mno-outline-atomics', '-idirafter', '$sdk/usr/include']
c_link_args = ['-L$sdk/usr/lib', '-L$sdk/lib', '-Wl,-rpath-link,$sdk/usr/lib:$sdk/lib', '-l:libc.musl-aarch64.so.1']
EOF
    export PATH="$work/tools/bin:$PATH"
    meson setup "$work/mpv-build" "$mpv" --cross-file "$work/aarch64.ini" \
        --auto-features=disabled --buildtype=release -Ddrm=enabled -Dgbm=enabled \
        -Dgl=enabled -Degl=enabled -Degl-drm=enabled -Dalsa=enabled \
        >"$work/mpv-build.log" 2>&1
    meson compile -C "$work/mpv-build" -j "$jobs" >>"$work/mpv-build.log" 2>&1
    runtime="$work/runtime"
    mkdir -p "$runtime"/{dev,proc,sys,tmp,etc,lib,usr/bin,usr/lib,usr/share}
    cp -a "$sdk/lib/ld-musl-aarch64.so.1" "$sdk/lib/libc.musl-aarch64.so.1" "$runtime/lib/"
    cp -a "$sdk"/usr/lib/*.so* "$sdk/usr/lib/dri" "$sdk/usr/lib/gbm" "$runtime/usr/lib/"
    cp -a "$sdk/usr/share/alsa" "$runtime/usr/share/"
    install -m755 "$work/mpv-build/mpv" "$ff/bin/ffmpeg" "$ff/bin/ffprobe" "$runtime/usr/bin/"
    tar -C "$runtime" -cf - . | gzip -1 >"$out/mpv-runtime.tar.gz"
    (cd "$out" && sha256sum mpv-runtime.tar.gz > SHA256SUMS)
fi
