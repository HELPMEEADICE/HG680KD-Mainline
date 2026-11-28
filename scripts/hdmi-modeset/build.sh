#!/usr/bin/env bash
set -euo pipefail

root=/home/hedc/hg680kd-mainline
seed="$root/out/arm64-7.3.6-usb-full/clean.oVBjwItr"
input=${1:?absolute modeset input directory required}
variant=${2:-modeset}
case "$variant" in
    modeset|modeset-locks|modeset-rxtag|4k60) ;;
    *) printf 'Unsupported modeset variant: %s\n' "$variant" >&2; exit 2 ;;
esac
work="$root/out/arm64-7.3.6-hdmi-$variant"
initramfs="$root/out/release-arm64-7.3.6-aiao/initramfs.cpio"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP='2025-12-01 00:00:00 UTC'
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1

exec 9>"$root/.build-lock"
flock 9
mkdir -p "$work"
build=$(mktemp -d "$work/clean.XXXXXXXX")
linux="$build/source"
out="$build/build"
release="$build/release"
mkdir "$out" "$release"
printf 'Modeset release directory: %s\n' "$release"
exec 3>&2
trap 'tail -100 "$release/build.log" >&3' ERR
{
    test -z "$(find "$out" -mindepth 1 -print -quit)"
    printf 'Empty kernel output directory: %s\n' "$out"
    test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
    "${cross}gcc" --version
    printf '%s  %s\n' \
        4e0eb22a725848e68e3215d694b0af286b03e71694fdf84ad4b798d4fbf6ee78 "$seed/build/.config" \
        b4b8c9dbef073903ac5ee415a91d281f451d1905fba53e92baf71850aabd9d85 "$seed/build/deploy/hi3798mv320-fiberhome-hg680kd-arm64-usb.dtb" \
        9fdac261df51a48e37d600c9c8587a94b25d1cb37fdebb1f3c2e37c5c353a4e5 "$initramfs" | sha256sum -c -
    cp -a --reflink=auto "$seed/source" "$linux"
    patch -d "$linux" -p1 --fuzz=0 < "$input/aiao.patch"
    install -m644 "$input/modeset-audio.h" "$linux/include/linux/hi3798-hdmi-modeset-audio.h"
    install -m644 "$input/modeset.c" "$linux/drivers/misc/hi3798-hdmi-modeset.c"
    if [[ $variant != modeset ]]; then
        patch -d "$linux" -p1 --fuzz=0 < "$input/locks.patch"
    fi
    if [[ $variant == modeset-rxtag || $variant == 4k60 ]]; then
        patch -d "$linux" -p1 --fuzz=0 < "$input/receiver-tags.patch"
    fi
    if [[ $variant == 4k60 ]]; then
        patch -d "$linux" -p1 --fuzz=0 < "$input/4k60.patch"
        modeset_dts="$build/modeset.dts"
        patch --fuzz=0 -o "$modeset_dts" "$input/modeset.dts" < "$input/4k60-dts.patch"
        install -m644 "$input/scdc.c" "$linux/drivers/misc/hi3798-hdmi-scdc.c"
        install -m644 "$input/scdc.h" "$linux/drivers/misc/hi3798-hdmi-scdc.h"
        printf '\nobj-y += hi3798-hdmi-scdc.o\n' >> "$linux/drivers/misc/Makefile"
    fi
    printf '\nobj-y += hi3798-hdmi-modeset.o\n' >> "$linux/drivers/misc/Makefile"
    cp "$seed/build/.config" "$out/.config"
    cmdline='console=ttyAMA0,115200n8 earlycon=pl011,mmio32,0xf8b00000 clk_ignore_unused cma=384M'
    "$linux/scripts/config" --file "$out/.config" --disable USB_STORAGE --disable USB_UAS \
        --disable MMC --enable WERROR --set-str INITRAMFS_SOURCE "$initramfs" \
        --set-str LOCALVERSION "-hg680kd-arm64-hdmi-$variant" --set-str CMDLINE "$cmdline"
    kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
    "${kernel[@]}" olddefconfig
    for symbol in DEBUG_FS CMDLINE_FORCE BLK_DEV_INITRD WERROR DRM_HI3798MV320_VDP \
        SND_HI3798MV320_AIAO VIDEO_HI3798MV320_VDEC; do
        grep -qx "CONFIG_$symbol=y" "$out/.config"
    done
    if grep -qE '^CONFIG_(USB_STORAGE|USB_UAS|MODULES|MTD|MMC)=[ym]$' "$out/.config"; then
        exit 1
    fi
    "${kernel[@]}" -j"${JOBS:-32}" Image scripts_dtc
    grep -qx "7.3.6-hg680kd-arm64-hdmi-$variant" "$out/include/config/kernel.release"
    board="$root/out/arm64-7.3.6-usb-full/project-8bcbf11/src"
    "${cross}gcc" -E -P -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I "$board" -I "$linux/include" -I "$linux/arch/arm64/boot/dts/hisilicon" \
        "${modeset_dts:-$input/modeset.dts}" -o "$out/modeset.pp.dts"
    "$out/scripts/dtc/dtc" -I dts -O dtb -o "$release/modeset.dtb" "$out/modeset.pp.dts"
    cp "$out/.config" "$release/kernel.config"
    python3 "$input/pack_arm64.py" --image "$out/arch/arm64/boot/Image" \
        --dtb "$release/modeset.dtb" --out "$release" --name boot.img
} >"$release/build.log" 2>&1
trap - ERR
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi
cd "$release"
sha256sum boot.img modeset.dtb kernel.config > SHA256SUMS
sha256sum -c SHA256SUMS
tail -2 build.log
