#!/usr/bin/env bash
set -euo pipefail

root=${1:-"$HOME/hg680kd-mainline"}
inputs=${2:-"$root/inputs"}
work="$root/out/arm64-7.3.6-jpgd"
linux="$work/source"
out="$work/build"
release="$root/out/release-arm64-7.3.6-jpgd"
baseline="$root/out/release-arm64-7.3.6-aiao"
board=hi3798mv320-fiberhome-hg680kd-arm64-jpgd
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
sysroot="$root/out/musl-arm64"
export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP='2025-12-01 00:00:00 UTC'
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1

exec 9>"$root/.build-lock"
flock 9
mkdir -p "$out" "$release"
if [[ ! -d $linux ]]; then
    cp -a --reflink=auto "$root/source/linux-7.3.6" "$linux"
fi
test ! -e "$linux/drivers/soc/hisilicon/hi3798-codec-probe.c"
dts="$linux/arch/arm64/boot/dts/hisilicon"
patchfile="$inputs/0016-media-add-Hi3798MV320-jpgd.patch"
if ! patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$patchfile" >/dev/null 2>&1; then
    patch -d "$linux" -p1 --forward --batch <"$patchfile"
fi
install -m644 "$inputs/hi3798mv320-jpgd.c" "$linux/drivers/media/platform/"
install -m644 "$inputs/crg-hi3798mv320-reset.c" "$linux/drivers/clk/hisilicon/"
install -m644 "$inputs/hisilicon,hi3798mv320-jpgd.h" "$linux/include/dt-bindings/clock/"
install -m644 "$inputs/hisilicon,hi3798mv320-jpgd.yaml" "$linux/Documentation/devicetree/bindings/media/"
install -m644 "$inputs/hi3798mv320-fiberhome-hg680kd-arm64.dts" "$inputs/$board.dts" \
    "$inputs/hi3798mv320-jpgd.dtsi" "$dts/"
if ! grep -Fq " $board.dtb" "$dts/Makefile"; then
    printf 'dtb-y += %s.dtb\n' "$board" >>"$dts/Makefile"
fi

test -s "$baseline/initramfs.cpio"
cp "$baseline/kernel.config" "$out/.config"
"$linux/scripts/config" --file "$out/.config" --enable VIDEO_HI3798MV320_JPGD \
    --set-str LOCALVERSION '-hg680kd-arm64-jpgd' \
    --set-str CMDLINE 'console=tty0 console=ttyAMA0,115200n8 earlycon=pl011,mmio32,0xf8b00000 clk_ignore_unused cma=384M' \
    --set-str INITRAMFS_SOURCE "$baseline/initramfs.cpio" \
    --disable MODULES --disable USB_STORAGE --disable USB_UAS
kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
{
    "${cross}gcc" --version
    "${kernel[@]}" olddefconfig
    for key in CC_IS_GCC CMDLINE_FORCE DMA_CMA VIDEO_HI3798MV320_JPGD V4L2_JPEG_HELPER \
            VIDEO_HI3798MV320_VDEC DRM_HI3798MV320_VDP SND_HI3798MV320_AIAO; do
        grep -qx "CONFIG_$key=y" "$out/.config"
    done
    grep -qx '# CONFIG_MODULES is not set' "$out/.config"
    "${kernel[@]}" -j"${JOBS:-$(nproc)}" Image "hisilicon/$board.dtb"
    "$sysroot/bin/musl-gcc" -static -O2 -Wall -Wextra -Werror \
        "$inputs/jpgd-test.c" -o "$release/jpgd-test"
} >"$release/build.log" 2>&1 || { tail -100 "$release/build.log"; exit 1; }
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi
grep -q 'jpgd_driver_init' "$out/System.map"
! grep -q 'hi3798_codec_probe\|codec_probe_init' "$out/System.map"
install -m644 "$out/arch/arm64/boot/Image" "$out/arch/arm64/boot/dts/hisilicon/$board.dtb" \
    "$out/include/config/kernel.release" "$release/"
install -m644 "$out/.config" "$release/kernel.config"
python3 "$(dirname "$0")/pack_arm64.py" --image "$release/Image" \
    --dtb "$release/$board.dtb" --out "$release" --name boot.img
cd "$release"
sha256sum boot.img "$board.dtb" kernel.config kernel.release jpgd-test >SHA256SUMS
sha256sum -c SHA256SUMS
