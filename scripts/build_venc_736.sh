#!/usr/bin/env bash
set -euo pipefail

root=${1:?remote workspace required}
work=${VENC_WORK_DIR:-"$root/out/venc"}
input=${2:-"$work/input"}
linux="$work/source"
baseline="$root/out/lima-dualpp"
out=${VENC_BUILD_DIR:-"$work/build"}
release=${VENC_RELEASE_DIR:-"$work/release"}
initramfs="$root/out/release-arm64-7.3.6-aiao/initramfs.cpio"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
board=hi3798mv320-fiberhome-hg680kd-arm64-venc
dts="$linux/arch/arm64/boot/dts/hisilicon"
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
    printf '%s  %s\n' \
        9fdac261df51a48e37d600c9c8587a94b25d1cb37fdebb1f3c2e37c5c353a4e5 "$initramfs" \
        b9e57e6508d2e180a8ccd04c91b4ff06a42a4727330c8aaeb91cc731fe3426f8 "$baseline/final/boot.img" | sha256sum -c -
    patch -d "$linux" -p1 --reverse --force --dry-run --silent \
        <"$input/0019-drm-lima-rearm-shared-pp-broadcast-irq.patch"
    patchfile="$input/0020-media-add-Hi3798MV320-venc.patch"
    if ! patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$patchfile" >/dev/null 2>&1; then
        patch -d "$linux" -p1 --forward --batch <"$patchfile"
    fi
    test ! -e "$linux/drivers/soc/hisilicon/probe.c"
    if grep -q 'crg-venc-probe.inc' "$linux/drivers/clk/hisilicon/crg-hi3798mv320-reset.c"; then
        exit 1
    fi
    install -m644 "$input"/hi3798mv320-venc{.c,-r002.h} "$linux/drivers/media/platform/"
    install -m644 "$input/crg-hi3798mv320-reset.c" "$linux/drivers/clk/hisilicon/"
    install -m644 "$input/hisilicon,hi3798mv320-jpgd.h" "$linux/include/dt-bindings/clock/"
    install -m644 "$input/hisilicon,hi3798mv320-venc.yaml" "$linux/Documentation/devicetree/bindings/media/"
    install -m644 "$input"/hi3798mv320-fiberhome-hg680kd-arm64*.dts "$input/hi3798mv320-jpgd.dtsi" "$dts/"

    cp "$baseline/final/kernel.config" "$out/.config"
    "$linux/scripts/config" --file "$out/.config" --enable VIDEO_HI3798MV320_VENC \
        --enable DEBUG_FS --enable CMA_DEBUGFS \
        --disable MTD --disable USB_STORAGE --disable USB_UAS --disable MODULES \
        --set-str INITRAMFS_SOURCE "$initramfs" --set-str LOCALVERSION '-hg680kd-arm64-venc-ippp'
    "${kernel[@]}" olddefconfig
    for key in CC_IS_GCC DRM_LIMA VIDEO_HI3798MV320_JPGD VIDEO_HI3798MV320_VDEC \
        VIDEO_HI3798MV320_VENC DRM_HI3798MV320_VDP SND_HI3798MV320_AIAO \
        DEBUG_FS CMA CMA_DEBUGFS DMA_CMA CMDLINE_FORCE; do
        grep -qx "CONFIG_$key=y" "$out/.config"
    done
    if grep -qE '^CONFIG_(USB_STORAGE|USB_UAS|MODULES|MTD)=[ym]$' "$out/.config"; then
        exit 1
    fi
    "${kernel[@]}" -j"${JOBS:-32}" Image scripts_dtc
    grep -q 'venc_driver_init' "$out/System.map"
    "${cross}gcc" -E -Wp,-MD,"$out/$board.d" -nostdinc -undef -D__DTS__ \
        -x assembler-with-cpp -I "$linux/include" "$dts/$board.dts" -o "$out/$board.pp.dts"
    "$out/scripts/dtc/dtc" -I dts -O dtb -o "$release/$board.dtb" "$out/$board.pp.dts"
    "$root/out/musl-arm64/bin/musl-gcc" -static -O2 -Wall -Wextra -Werror \
        "$input/venc-test.c" -o "$release/venc-test"
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
sha256sum boot.img "$board.dtb" kernel.config kernel.release venc-test >SHA256SUMS
sha256sum -c SHA256SUMS
