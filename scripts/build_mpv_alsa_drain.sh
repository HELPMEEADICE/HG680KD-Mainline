#!/usr/bin/env bash
set -euo pipefail

root=${HG680KD_BUILD_ROOT:-"$HOME/hg680kd-mainline"}
patch=$(realpath "${1:?path to mpv-alsa-native-drain.patch required}")
base="$root/out/arm64-7.3.6-mpv-drm"
output="$root/out/arm64-7.3.6-aiao-drain"
mkdir -p "$output"
work=$(mktemp -d "$output/mpv.XXXXXXXX")
export PATH="$base/tools/bin:$PATH"
export REALGCC="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-gcc"

printf 'MPV ALSA drain build directory: %s\n' "$work"
test "$(git -C "$base/mpv" rev-parse HEAD)" = 41f6a645068483470267271e1d09966ca3b9f413
cp -a "$base/mpv" "$work/source"
git -C "$work/source" apply "$patch"
trap 'tail -50 "$work/build.log" >&2' ERR
{
    meson setup "$work/build" "$work/source" --cross-file "$base/aarch64.ini" \
        --auto-features=disabled --buildtype=release -Ddrm=enabled -Dgbm=enabled \
        -Dgl=enabled -Degl=enabled -Degl-drm=enabled -Dalsa=enabled
    meson compile -C "$work/build" -j "${JOBS:-8}"
} >"$work/build.log" 2>&1
trap - ERR
sha256sum "$work/build/mpv"
