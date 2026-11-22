#!/usr/bin/env bash
set -euo pipefail

root="$HOME/hg680kd-mainline"
linux="$root/source/linux-7.3.5"
original="$root/out/release-arm64-7.3.5-armbian-usb"
validated="$root/out/release-arm64-7.3.5-vdec"
release=7.3.5-hg680kd-arm64-armbian-usb-vdec
out="$root/out/arm64-7.3.5-armbian-usb-vdec"
package="$out/deploy"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP='2025-12-01 00:00:00 UTC'
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1
mkdir -p "$package"
exec 9>"$root/.build-lock"
flock 9

echo '85c49679a487e2ed26a1b7eb4000a1bf317f0359347cb3f671112c78d5e8b6c3  boot.img' | (cd "$validated" && sha256sum -c -)
(cd "$linux/drivers/media/platform" && sha256sum -c - <<'EOF'
1479264e9c3d318902f701622cbcc0a9a7417c5522a86fb6c26a86c1557f5814  hi3798mv320-vdec-core.c
9aed0cb9cb01cb033fc6519448df823d73f96e2281e2a0e8e69e88c2115c1710  hi3798mv320-vdec-neon.S
3c2d0723ef001de3efac292ae10bce03ec4c61ac0e5026144ffbf5eb6adbf010  hi3798mv320-vdec-cabac.h
EOF
)

cp "$original/kernel.config" "$out/.config"
cmdline=$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$out/.config")
[[ "$cmdline" == *'root=PARTLABEL=HG680KD_ROOT rootfstype=f2fs '* ]]
cmdline+=" cma=384M"
config=("$linux/scripts/config" --file "$out/.config")
"${config[@]}" --set-str LOCALVERSION "${release#7.3.5}" --set-str CMDLINE "$cmdline" \
    --module MEDIA_SUPPORT --enable MEDIA_PLATFORM_SUPPORT --enable MEDIA_PLATFORM_DRIVERS \
    --module VIDEO_DEV --enable V4L_MEM2MEM_DRIVERS --module VIDEO_HI3798MV320_VDEC \
    --enable CMA --enable DMA_CMA
if [[ ${1:-} == compact ]]; then
    "${config[@]}" --disable DYNAMIC_DEBUG --enable DYNAMIC_DEBUG_CORE
fi
kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
"${kernel[@]}" olddefconfig
for symbol in CMA DMA_CMA CMDLINE_FORCE USB_STORAGE USB_XHCI_HI3798MV320 F2FS_FS F2FS_FS_ZSTD HIX5HD2_GMAC MODULES; do
    grep -qx "CONFIG_$symbol=y" "$out/.config"
done
for symbol in VIDEO_HI3798MV320_VDEC VIDEO_DEV MEDIA_SUPPORT BT_HCIUART; do
    grep -qx "CONFIG_$symbol=m" "$out/.config"
done
grep -qx 'CONFIG_INITRAMFS_SOURCE=""' "$out/.config"
"${kernel[@]}" -j"${JOBS:-24}" Image modules scripts_dtc

dts_dir="$linux/arch/arm64/boot/dts/hisilicon"
board=hi3798mv320-fiberhome-hg680kd-arm64-vdec
cat > "$out/$board-usb.dts" <<EOF
#include "$board.dts"
/ {
    chosen {
        bootargs = "$cmdline";
    };
};
EOF
"${cross}gcc" -E -P -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$linux/scripts/dtc/include-prefixes" -I "$dts_dir" "$out/$board-usb.dts" \
    -o "$out/$board-usb.pp.dts"
"$out/scripts/dtc/dtc" -I dts -O dtb -o "$package/$board-usb.dtb" "$out/$board-usb.pp.dts"
cp "$out/arch/arm64/boot/Image" "$package/Image"
cp "$out/.config" "$package/kernel.config"
python3 "$root/scripts/pack_arm64.py" --image "$package/Image" --dtb "$package/$board-usb.dtb" \
    --out "$package" --name boot.img

old_size=$(stat -c %s "$root/out/boot-7.3.5-armbian-usb.img")
new_size=$(stat -c %s "$package/boot.img")
printf 'original image bytes=%s, replacement bytes=%s\n' "$old_size" "$new_size"
[[ "$old_size" == 15245384 && "$new_size" -le "$old_size" ]]
truncate -s "$old_size" "$package/boot.img"
(cd "$package" && sha256sum boot.img > BOOT-SHA256SUMS)
(cd "$package" && sha256sum boot.img Image ./*.dtb kernel.config > SHA256SUMS && cat SHA256SUMS)

stage=$(mktemp -d "$out/modules-stage.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
"${kernel[@]}" INSTALL_MOD_PATH="$stage" INSTALL_MOD_STRIP=1 modules_install
[[ -d "$stage/lib/modules/$release" ]]
rm -f "$stage/lib/modules/$release/"{build,source}
depmod -b "$stage" "$release"
grep -E 'hi3798mv320.*vdec' "$stage/lib/modules/$release/modules.alias"
(cd "$stage/lib/modules/$release" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$package/modules.SHA256SUMS"
tar -C "$stage/lib/modules" -czf "$package/modules.tar.gz" "$release"
tar -C "$package" -czf "$out/usb-vdec-deploy.tar.gz" boot.img Image "$board-usb.dtb" \
    kernel.config SHA256SUMS modules.tar.gz modules.SHA256SUMS
