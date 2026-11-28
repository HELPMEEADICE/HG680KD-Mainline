#!/usr/bin/env bash
set -euo pipefail

root=${1:-"$HOME/hg680kd-mainline"}
inputs=${2:-"$root/out/lima-dualpp/input"}
baseline="$root/out/arm64-7.3.6-usb-display"
work="$root/out/lima-dualpp"
linux="$work/source"
out="$work/build"
release="$work/final"
initramfs="$root/out/release-arm64-7.3.6-aiao/initramfs.cpio"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
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
final_patch="$inputs/0019-drm-lima-rearm-shared-pp-broadcast-irq.patch"
if ! patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$final_patch" >/dev/null 2>&1; then
    mask_patch="$inputs/0018-drm-lima-match-pp-broadcast-to-job.patch"
    if ! patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$mask_patch" >/dev/null 2>&1; then
        patch -d "$linux" -p1 --forward --batch <"$mask_patch"
    fi
    patch -d "$linux" -p1 --forward --batch <"$final_patch"
fi
printf '%s  %s\n' 9fdac261df51a48e37d600c9c8587a94b25d1cb37fdebb1f3c2e37c5c353a4e5 "$initramfs" | sha256sum -c -
cp "$baseline/build/.config" "$out/.config"
"$linux/scripts/config" --file "$out/.config" --disable USB_STORAGE --disable USB_UAS --disable MODULES \
    --set-str INITRAMFS_SOURCE "$initramfs" --set-str LOCALVERSION '-hg680kd-arm64-lima-ram' \
    --set-str CMDLINE 'console=tty0 console=ttyAMA0,115200n8 earlycon=pl011,mmio32,0xf8b00000 clk_ignore_unused cma=384M'

board=hi3798mv320-fiberhome-hg680kd-arm64-usb
cat "$baseline/build/$board.pp.dts" >"$release/dual.dts"
cat >>"$release/dual.dts" <<'EOF'
&{/soc/usb@f98a0000} { status = "disabled"; };
&gpu {
    interrupts = <0 94 4>, <0 94 4>, <0 94 4>, <0 94 4>,
                 <0 94 4>, <0 94 4>, <0 94 4>, <0 94 4>;
    interrupt-names = "gp", "gpmmu", "pp0", "ppmmu0", "pp1", "ppmmu1", "pmu", "pp";
};
EOF

kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
{
    test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
    "${cross}gcc" --version
    "${kernel[@]}" olddefconfig
    for key in CC_IS_GCC CMDLINE_FORCE DRM_LIMA VIDEO_HI3798MV320_JPGD VIDEO_HI3798MV320_VDEC DRM_HI3798MV320_VDP SND_HI3798MV320_AIAO; do
        grep -qx "CONFIG_$key=y" "$out/.config"
    done
    for key in USB_STORAGE MODULES; do
        grep -qx "# CONFIG_$key is not set" "$out/.config"
    done
    ! grep -qE '^CONFIG_USB_UAS=[ym]$' "$out/.config"
    "${kernel[@]}" -j"${JOBS:-32}" Image scripts_dtc
    "$out/scripts/dtc/dtc" -I dts -O dtb -o "$release/dual.dtb" "$release/dual.dts"
    "$root/out/musl-arm64/bin/musl-gcc" -static -O2 -Wall -Wextra -Werror \
        "$inputs/lima-test.c" -o "$release/lima-test"
} >"$release/build.log" 2>&1 || { tail -100 "$release/build.log"; exit 1; }
if grep -iEn '(^|[[:space:]])warning[: (]' "$release/build.log"; then
    exit 1
fi

cp "$out/.config" "$release/kernel.config"
cp "$out/include/config/kernel.release" "$release/kernel.release"
python3 "$inputs/pack_arm64.py" --image "$out/arch/arm64/boot/Image" \
    --dtb "$release/dual.dtb" --out "$release" --name boot.img
cd "$release"
sha256sum boot.img dual.dtb kernel.config kernel.release lima-test >SHA256SUMS
sha256sum -c SHA256SUMS
