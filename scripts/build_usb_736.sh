#!/usr/bin/env bash
set -euo pipefail
root=${HG680KD_BUILD_ROOT:-"$HOME/hg680kd-mainline"}
seed="$root/out/arm64-7.3.6-kvm-test"
work="$root/out/arm64-7.3.6-usb-full"
firmware="$root/out/arm64-7.3.6-usb-2026-11-28/firmware"
project=$(cd "$(dirname "$0")/.." && pwd)
board=hi3798mv320-fiberhome-hg680kd-arm64-usb
board_dts=${1:-"$project/src/$board.dts"}
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
pack="$out/deploy"
mkdir -p "$pack"
printf 'Clean kernel output directory: %s\n' "$out"
test -z "$(find "$out" -mindepth 1 -maxdepth 1 ! -name deploy -print -quit)"
test "$("${cross}gcc" -dumpfullversion)" = 14.2.0
"${cross}gcc" --version
printf '%s  %s\n' \
    8a82e29b2aab122f0e911cd5882f441bde7173f4ddf57e7f88aa5beb9762b227 "$seed/.config" \
    1157518b5f739bc455374f6060bd15658d031a9c5277329d3454fa2a15dd8553 "$seed/hi3798mv320-fiberhome-hg680kd-arm64-kvm-test.dtb" \
    846ee9c3b2b4e45f9317d7307753374bd1aa401de9586460684b6b6b543047fe "$firmware/rtl_bt/rtl8761btv_vat_fw.bin" \
    7309d5f317fb80f630c6dca08c09559cf2db834801b86ded9ff0b22416a5e07c "$firmware/rtl_bt/rtl8761btv_vat_config.bin" | sha256sum -c -
cp -a --reflink=auto "$root/source/linux-7.3.6" "$linux"
apply_patch_if_needed() {
    local patch_file="$project/patches/$1"
    if ! patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$patch_file" >/dev/null 2>&1; then
        patch -d "$linux" -p1 --forward --batch <"$patch_file"
    fi
}
apply_patch_if_needed 0016-media-add-Hi3798MV320-jpgd.patch
lima_final=0019-drm-lima-rearm-shared-pp-broadcast-irq.patch
# The final patch edits earlier Lima hunks, so check it before its prerequisites.
if ! patch -d "$linux" -p1 --reverse --force --dry-run --silent <"$project/patches/$lima_final" >/dev/null 2>&1; then
    apply_patch_if_needed 0017-drm-lima-handle-shared-pp-irq-once.patch
    apply_patch_if_needed 0018-drm-lima-match-pp-broadcast-to-job.patch
    patch -d "$linux" -p1 --forward --batch <"$project/patches/$lima_final"
fi
apply_patch_if_needed 0020-media-add-Hi3798MV320-venc.patch
install -m644 "$project/src/hi3798mv320-vdp.c" "$linux/drivers/gpu/drm/"
install -m644 "$project/src/hi3798mv320-jpgd.c" "$linux/drivers/media/platform/"
install -m644 "$project/src/hi3798mv320-vdec.c" "$linux/drivers/media/platform/hi3798mv320-vdec-core.c"
install -m644 "$project/src"/hi3798mv320-venc{.c,-r002.h} "$linux/drivers/media/platform/"
install -m644 "$project/src/hisilicon,hi3798mv320-venc.yaml" "$linux/Documentation/devicetree/bindings/media/"
install -m644 "$project/src/crg-hi3798mv320-reset.c" "$linux/drivers/clk/hisilicon/"
install -m644 "$project/src/hisilicon,hi3798mv320-jpgd.h" "$linux/include/dt-bindings/clock/"
install -m644 "$project/src/hi3798mv320-fiberhome-hg680kd-arm64.dts" \
    "$linux/arch/arm64/boot/dts/hisilicon/"
# Register the native display card before Lima's non-modesetting primary node.
drm_makefile="$linux/drivers/gpu/drm/Makefile"
sed -i '/^obj-\$(CONFIG_DRM_HI3798MV320_VDP)/d; /^obj-\$(CONFIG_DRM_LIMA)/i obj-$(CONFIG_DRM_HI3798MV320_VDP) += hi3798mv320-vdp.o' \
    "$drm_makefile"
cmdline=$(sed -nE 's/^[[:space:]]*bootargs = "(.*)";$/\1/p' "$board_dts")
[[ "$cmdline" == *' root=PARTLABEL=HG680KD_ROOT rootfstype=f2fs '* && "$cmdline" == *' rootwait cma=384M' ]]
cp "$seed/.config" "$out/.config"
config=("$linux/scripts/config" --file "$out/.config")
"${config[@]}" --set-str INITRAMFS_SOURCE '' --set-str CMDLINE "$cmdline" --set-str LOCALVERSION '-hg680kd-arm64-usb' \
    --enable SYSVIPC --enable POSIX_MQUEUE --enable BPF_SYSCALL --enable CGROUPS \
    --enable CGROUP_SCHED --enable FAIR_GROUP_SCHED --enable CFS_BANDWIDTH --enable CGROUP_PIDS \
    --enable CGROUP_FREEZER --enable CGROUP_DEVICE --enable CGROUP_CPUACCT --enable CGROUP_BPF \
    --enable UTS_NS --enable IPC_NS --enable USER_NS --enable PID_NS --enable NET_NS \
    --enable SECCOMP --enable UNIX --enable INOTIFY_USER --enable FANOTIFY --enable AUTOFS_FS \
    --enable TMPFS_XATTR --enable TMPFS_POSIX_ACL --enable NLS_UTF8 --enable FUSE_FS --enable BINFMT_MISC \
    --enable MAGIC_SYSRQ_SERIAL --set-val MAGIC_SYSRQ_DEFAULT_ENABLE 0xb0 --enable IPV6 --disable IPV6_SIT \
    --disable DYNAMIC_DEBUG --enable DYNAMIC_DEBUG_CORE \
    --set-str EXTRA_FIRMWARE_DIR "$firmware" \
    --set-str EXTRA_FIRMWARE 'rtl_bt/rtl8761btv_vat_fw.bin rtl_bt/rtl8761btv_vat_config.bin'
"$linux/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" \
    "$project/src/arm64-7.3.6-usb-display.config"
kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
"${kernel[@]}" olddefconfig > "$build/build.log" 2>&1
for symbol in VIRTUALIZATION KVM CMDLINE_FORCE CMA CMA_DEBUGFS DMA_CMA EFI_PARTITION DEVTMPFS_MOUNT F2FS_FS F2FS_FS_ZSTD VFAT_FS USB_STORAGE USB_XHCI_HI3798MV320 VIDEO_HI3798MV320_VDEC VIDEO_HI3798MV320_JPGD VIDEO_HI3798MV320_VENC DRM_HI3798MV320_VDP SND_HI3798MV320_AIAO CGROUPS UNIX INOTIFY_USER IPV6 MAGIC_SYSRQ_SERIAL DRM_LIMA DRM_CLIENT_SETUP DRM_FBDEV_EMULATION DRM_CLIENT_DEFAULT_FBDEV FRAMEBUFFER_CONSOLE; do
    grep -qx "CONFIG_$symbol=y" "$out/.config" || { echo "Missing built-in CONFIG_$symbol" >&2; exit 1; }
done
grep -Fx 'CONFIG_INITRAMFS_SOURCE=""' "$out/.config"
grep -Fx "CONFIG_CMDLINE=\"$cmdline\"" "$out/.config"
if ! "${kernel[@]}" -j"${JOBS:-24}" Image scripts_dtc >> "$build/build.log" 2>&1; then
    tail -100 "$build/build.log"
    exit 1
fi
grep -qw venc_driver_init "$out/System.map"
cp "$board_dts" "$pack/$board.dts"
"${cross}gcc" -E -P -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$(dirname "$board_dts")" -I "$seed" -I "$linux/include" -I "$linux/arch/arm64/boot/dts/hisilicon" \
    "$pack/$board.dts" -o "$out/$board.pp.dts"
"$out/scripts/dtc/dtc" -I dts -O dtb -p 0 -o "$pack/$board.dtb" "$out/$board.pp.dts" 2>>"$build/build.log"
if grep -iEn '(^|[[:space:]])warning[: (]' "$build/build.log"; then
    exit 1
fi
cp "$build/build.log" "$pack/"
cp "$out/arch/arm64/boot/Image" "$pack/Image"
cp "$out/.config" "$pack/kernel.config"
cp "$out/include/config/kernel.release" "$pack/kernel.release"
python3 "$project/scripts/pack_arm64.py" --image "$pack/Image" --dtb "$pack/$board.dtb" --out "$pack" --name boot.img
bytes=$(stat -c %s "$pack/boot.img")
printf 'Unpadded image: %s bytes; fixed file size: 15245384 bytes\n' "$bytes"
[[ "$bytes" -le 15245384 ]]
truncate -s 15245384 "$pack/boot.img"
cd "$pack"
sha256sum boot.img > BOOT-SHA256SUMS
sha256sum boot.img Image "$board.dtb" "$board.dts" kernel.config kernel.release > SHA256SUMS
sha256sum -c SHA256SUMS
cat SHA256SUMS
printf 'USB build directory: %s\n' "$pack"
