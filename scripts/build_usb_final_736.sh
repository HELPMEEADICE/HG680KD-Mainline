#!/usr/bin/env bash
set -euo pipefail

root=${HG680KD_BUILD_ROOT:-"$HOME/hg680kd-mainline"}
project=$(cd "$(dirname "$0")/.." && pwd)
seed="$root/out/arm64-7.3.6-aiao-drain/clean.3AlNpmSD"
work="$root/out/arm64-7.3.6-usb-final"
board=hi3798mv320-fiberhome-hg680kd-arm64-usb-final
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
printf 'USB final release directory: %s\n' "$release"
trap 'tail -80 "$release/build.log" >&2' ERR
{
    test -z "$(find "$out" -mindepth 1 -print -quit)"
    printf 'Empty kernel output directory: %s\n' "$out"
    test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
    "${cross}gcc" --version
    printf '%s  %s\n' \
        222b4f5879dd92470c9e6a16978692423d9d469683293822100d62f794a916a8 "$seed/release/boot.img" \
        9e8643c4c6eae7435b715d7fe810ad16075f9cccd56893f1e5dafdc43587e8ac "$seed/release/hdmi-drm.dtb" \
        37e2bfd233cd35a81648187aa6a08a61dc4288ae1f7a7b883c188df24e657bd3 "$seed/release/kernel.config" |
        sha256sum -c -
    cp -a --reflink=auto "$seed/source" "$linux"
    for driver in hi3798mv320-vdp.c hi3798mv320-hdmi.c hi3798mv320-hdmi.h; do
        cmp "$project/src/$driver" "$linux/drivers/gpu/drm/$driver"
    done
    for driver in hi3798mv320-jpgd.c hi3798mv320-venc.c hi3798mv320-venc-r002.h; do
        cmp "$project/src/$driver" "$linux/drivers/media/platform/$driver"
    done
    cmp "$project/src/hi3798mv320-vdec.c" "$linux/drivers/media/platform/hi3798mv320-vdec-core.c"
    cmp "$project/src/hi3798mv320-aiao.c" "$linux/sound/drivers/hi3798mv320-aiao.c"
    cmp "$project/src/xhci-hi3798mv320.c" "$linux/drivers/usb/host/xhci-hi3798mv320.c"

    cp "$seed/release/kernel.config" "$out/.config"
    cmdline=$(sed -nE 's/^[[:space:]]*bootargs = "(.*)";$/\1/p' "$project/src/$board.dts")
    [[ "$cmdline" == *' root=PARTLABEL=HG680KD_ROOT rootfstype=f2fs '* && "$cmdline" == *' rootwait cma=384M' ]]
    "$linux/scripts/config" --file "$out/.config" --set-str INITRAMFS_SOURCE '' \
        --set-str CMDLINE "$cmdline" --set-str LOCALVERSION '-hg680kd-arm64-usb-final' \
        --enable USB_STORAGE --disable DEVMEM
    kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
    "${kernel[@]}" olddefconfig
    for symbol in WERROR CMDLINE_FORCE KVM CMA CMA_DEBUGFS DMA_CMA EFI_PARTITION DEVTMPFS_MOUNT \
        F2FS_FS F2FS_FS_ZSTD VFAT_FS BLK_DEV_SD USB_STORAGE USB_XHCI_HI3798MV320 \
        DRM_HI3798MV320_VDP DRM_LIMA DRM_FBDEV_EMULATION FRAMEBUFFER_CONSOLE SND_HI3798MV320_AIAO \
        VIDEO_HI3798MV320_VDEC VIDEO_HI3798MV320_JPGD VIDEO_HI3798MV320_VENC; do
        grep -qx "CONFIG_$symbol=y" "$out/.config"
    done
    if grep -qE '^CONFIG_(USB_UAS|MODULES|MTD|MMC|NVMEM|DEVMEM)=[ym]$' "$out/.config"; then
        exit 1
    fi
    grep -Fx 'CONFIG_INITRAMFS_SOURCE=""' "$out/.config"
    grep -Fx "CONFIG_CMDLINE=\"$cmdline\"" "$out/.config"
    "${kernel[@]}" -j"${JOBS:-32}" Image scripts_dtc
    grep -qx '7.3.6-hg680kd-arm64-usb-final' "$out/include/config/kernel.release"

    compile_dtb() {
        "${cross}gcc" -E -P -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
            -I "$project/src" -I "$linux/include" -I "$linux/arch/arm64/boot/dts/hisilicon" \
            "$project/src/$1.dts" -o "$out/$1.pp.dts"
        "$out/scripts/dtc/dtc" -I dts -O dtb -o "$2" "$out/$1.pp.dts"
    }
    compile_dtb hi3798mv320-fiberhome-hg680kd-arm64-hdmi-drm "$out/ram.dtb"
    cmp "$out/ram.dtb" "$seed/release/hdmi-drm.dtb"
    compile_dtb "$board" "$release/$board.dtb"
    cp "$project/src/$board.dts" "$release/"
    cp "$out/.config" "$release/kernel.config"
    cp "$out/include/config/kernel.release" "$release/kernel.release"
    cp "$out/arch/arm64/boot/Image" "$release/Image"
    python3 "$project/scripts/pack_arm64.py" --image "$release/Image" \
        --dtb "$release/$board.dtb" --out "$release" --name boot.img
    bytes=$(stat -c %s "$release/boot.img")
    printf 'Unpadded legacy image: %s bytes; fixed FAT file size: 15245384 bytes\n' "$bytes"
    test "$bytes" -le 15245384
    truncate -s 15245384 "$release/boot.img"
} >"$release/build.log" 2>&1
trap - ERR
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi
cd "$release"
sha256sum boot.img > BOOT-SHA256SUMS
sha256sum boot.img Image "$board.dtb" "$board.dts" kernel.config kernel.release > SHA256SUMS
sha256sum -c SHA256SUMS
cat SHA256SUMS
