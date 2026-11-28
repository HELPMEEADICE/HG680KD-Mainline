#!/usr/bin/env bash
set -euo pipefail

root=${1:?remote workspace required}
work="$root/out/arm64-7.3.6-vdec-ext"
input="$work/input"
baseline="$root/out/venc-ffmpeg"
linux="$work/source"
out=${2:?kernel output directory required}
release=${3:?release directory required}
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
board=hi3798mv320-fiberhome-hg680kd-arm64-venc
export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP='2025-12-01 00:00:00 UTC'
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1

exec 9>"$root/.build-lock"
flock 9
mkdir -p "$out" "$release"
if [[ ! -d $linux ]]; then
    cp -a --reflink=auto "$baseline/source" "$linux"
fi
kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
exec 3>&2
trap 'tail -100 "$release/build.log" >&3' ERR
{
    test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
    "${cross}gcc" --version
    if [[ -z $(find "$out" -mindepth 1 -print -quit) ]]; then
        echo 'Kernel output directory is empty'
    fi
    (
        cd "$baseline/release"
        sha256sum -c SHA256SUMS
    )
    cp "$baseline/release/kernel.config" "$out/.config"
    "$linux/scripts/config" --file "$out/.config" --set-str LOCALVERSION -hg680kd-arm64-vdec-ext
    install -m644 "$input/hi3798mv320-vdec.c" "$linux/drivers/media/platform/hi3798mv320-vdec-core.c"
    "${kernel[@]}" olddefconfig
    for key in CC_IS_GCC DRM_LIMA VIDEO_HI3798MV320_JPGD VIDEO_HI3798MV320_VDEC \
        VIDEO_HI3798MV320_VENC DRM_HI3798MV320_VDP SND_HI3798MV320_AIAO \
        DEBUG_FS CMA CMA_DEBUGFS DMA_CMA CMDLINE_FORCE; do
        grep -qx "CONFIG_$key=y" "$out/.config"
    done
    if grep -qE '^CONFIG_(USB_STORAGE|USB_UAS|MODULES|MTD)=[ym]$' "$out/.config"; then
        exit 1
    fi
    "${kernel[@]}" -j"${JOBS:-32}" Image
    cp "$baseline/release/$board.dtb" "$release/"
    cp "$out/.config" "$release/kernel.config"
    cp "$out/include/config/kernel.release" "$release/kernel.release"
    python3 "$input/pack_arm64.py" --image "$out/arch/arm64/boot/Image" \
        --dtb "$release/$board.dtb" --out "$release" --name boot.img
} >"$release/build.log" 2>&1
trap - ERR
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi
cd "$release"
sha256sum boot.img "$board.dtb" kernel.config kernel.release > SHA256SUMS
sha256sum -c SHA256SUMS
