#!/usr/bin/env bash
set -euo pipefail

root=${1:?remote workspace required}
work="$root/out/venc-probe"
baseline="$root/out/lima-dualpp"
input="$work/input"
linux="$work/source"
out="${VENC_BUILD_DIR:-$work/build}"
release="${VENC_RELEASE_DIR:-$work/release}"
initramfs="$root/out/release-arm64-7.3.6-aiao/initramfs.cpio"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP='2025-12-01 00:00:00 UTC'
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1

exec 9>"$root/.build-lock"
flock 9
mkdir -p "$work" "$out" "$release"
if [[ ! -d $linux ]]; then
    cp -a --reflink=auto "$baseline/source" "$linux"
fi

kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
{
    test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
    "${cross}gcc" --version
    printf '%s  %s\n' \
        9fdac261df51a48e37d600c9c8587a94b25d1cb37fdebb1f3c2e37c5c353a4e5 "$initramfs" \
        b9e57e6508d2e180a8ccd04c91b4ff06a42a4727330c8aaeb91cc731fe3426f8 "$baseline/final/boot.img" | sha256sum -c -
    patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$input/0019-drm-lima-rearm-shared-pp-broadcast-irq.patch"
    cp "$baseline/final/kernel.config" "$out/.config"
    "$linux/scripts/config" --file "$out/.config" --enable DEBUG_FS \
        --disable MTD --disable USB_STORAGE --disable USB_UAS --disable MODULES \
        --set-str INITRAMFS_SOURCE "$initramfs" --set-str LOCALVERSION '-hg680kd-arm64-venc-probe'
    "${kernel[@]}" olddefconfig
    for key in DRM_LIMA VIDEO_HI3798MV320_JPGD VIDEO_HI3798MV320_VDEC DRM_HI3798MV320_VDP SND_HI3798MV320_AIAO DEBUG_FS CMA DMA_CMA CMDLINE_FORCE; do
        grep -qx "CONFIG_$key=y" "$out/.config"
    done
    ! grep -qE '^CONFIG_(USB_STORAGE|USB_UAS|MODULES|MTD)=[ym]$' "$out/.config"
    cp "$input"/{probe.c,r002-registers.h} "$linux/drivers/soc/hisilicon/"
    cp "$input/crg-venc-probe.inc" "$linux/drivers/clk/hisilicon/"
    grep -qxF 'obj-y += probe.o' "$linux/drivers/soc/hisilicon/Makefile" ||
        printf '\nobj-y += probe.o\n' >>"$linux/drivers/soc/hisilicon/Makefile"
    grep -qxF '#include "crg-venc-probe.inc"' "$linux/drivers/clk/hisilicon/crg-hi3798mv320-reset.c" ||
        printf '\n#include "crg-venc-probe.inc"\n' >>"$linux/drivers/clk/hisilicon/crg-hi3798mv320-reset.c"
    "${kernel[@]}" -j"${JOBS:-32}" Image scripts_dtc
    cat "$baseline/final/dual.dts" "$input/probe.dtsi" >"$release/probe.dts"
    "$out/scripts/dtc/dtc" -I dts -O dtb -o "$release/probe.dtb" "$release/probe.dts"
    cp "$out/.config" "$release/kernel.config"
    cp "$out/include/config/kernel.release" "$release/kernel.release"
    python3 "$input/pack_arm64.py" --image "$out/arch/arm64/boot/Image" \
        --dtb "$release/probe.dtb" --out "$release" --name boot.img
} >"$release/build.log" 2>&1 || { tail -100 "$release/build.log"; exit 1; }
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi

cd "$release"
sha256sum boot.img probe.dtb kernel.config kernel.release >SHA256SUMS
sha256sum -c SHA256SUMS
