#!/usr/bin/env bash
set -euo pipefail

root=${HG680KD_BUILD_ROOT:-"$HOME/hg680kd-mainline"}
driver=${1:?path to hi3798mv320-aiao.c required}
seed="$root/out/arm64-7.3.6-hdmi-drm/clean.dFVfes5N"
work="$root/out/arm64-7.3.6-aiao-drain"
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
printf 'AIAO drain release directory: %s\n' "$release"
trap 'tail -80 "$release/build.log" >&2' ERR
{
    test -z "$(find "$out" -mindepth 1 -print -quit)"
    printf 'Empty kernel output directory: %s\n' "$out"
    test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
    "${cross}gcc" --version
    printf '%s  %s\n' \
        570086220b4454e4e361170575fbee11b63e7c16e972aabdcdff93a52858ecee "$seed/release/boot.img" \
        9e8643c4c6eae7435b715d7fe810ad16075f9cccd56893f1e5dafdc43587e8ac "$seed/release/hdmi-drm.dtb" \
        37e2bfd233cd35a81648187aa6a08a61dc4288ae1f7a7b883c188df24e657bd3 "$seed/release/kernel.config" \
        861fa1ef3a0fab5223277ea1f9ce8dac7f4f3e597febcd7cc7dcca1c41c0a1cb "$seed/source/sound/drivers/hi3798mv320-aiao.c" \
        9fdac261df51a48e37d600c9c8587a94b25d1cb37fdebb1f3c2e37c5c353a4e5 "$root/out/release-arm64-7.3.6-aiao/initramfs.cpio" |
        sha256sum -c -
    cp -a --reflink=auto "$seed/source" "$linux"
    sha256sum "$driver"
    install -m644 "$driver" "$linux/sound/drivers/hi3798mv320-aiao.c"
    cp "$seed/release/kernel.config" "$out/.config"
    kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
    "${kernel[@]}" olddefconfig
    cmp "$out/.config" "$seed/release/kernel.config"
    grep -qx 'CONFIG_SND_HI3798MV320_AIAO=y' "$out/.config"
    grep -qx 'CONFIG_WERROR=y' "$out/.config"
    if grep -qE '^CONFIG_(USB_STORAGE|USB_UAS|MODULES|MTD|MMC)=[ym]$' "$out/.config"; then
        exit 1
    fi
    "${kernel[@]}" -j"${JOBS:-32}" Image
    grep -qx '7.3.6-hg680kd-arm64-hdmi-drm' "$out/include/config/kernel.release"
    cp "$out/.config" "$release/kernel.config"
    cp "$seed/release/hdmi-drm.dtb" "$release/"
    python3 "$root/scripts/pack_arm64.py" --image "$out/arch/arm64/boot/Image" \
        --dtb "$release/hdmi-drm.dtb" --out "$release" --name boot.img
} >"$release/build.log" 2>&1
trap - ERR
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi
cd "$release"
sha256sum boot.img hdmi-drm.dtb kernel.config > SHA256SUMS
sha256sum -c SHA256SUMS
tail -2 build.log
