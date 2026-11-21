#!/usr/bin/env bash
set -euo pipefail

root=${1:-"$HOME/hg680kd-mainline"}
inputs=${2:-"$root/inputs"}
variant=${3:-core}
jobs=${JOBS:-$(nproc)}
linux_ver=${LINUX_VERSION:-6.18.64}
base_variant=$variant
if [[ $variant == 7.2.5-* ]]; then
    linux_ver=7.2.5
    base_variant=${variant#7.2.5-}
elif [[ $variant == 7.3.5-* ]]; then
    linux_ver=7.3.5
    base_variant=${variant#7.3.5-}
fi
vpu_probe=
vpu_h264_poc=
if [[ $variant == 7.3.5-vpu-probe || $variant == 7.3.5-vpu-h264-poc ]]; then
    base_variant=all-2g-hdmi-bt-dvfs
    vpu_probe=y
fi
if [[ $variant == 7.3.5-vpu-h264-poc ]]; then
    vpu_h264_poc=y
fi
linux="$root/source/linux-$linux_ver"
busybox="$root/source/busybox-1.37.0"
musl="$root/source/musl-1.2.5"
sysroot="$root/out/musl-arm64"
out="$root/out/arm64-$variant"
bbout="$root/out/busybox-arm64"
cross="$root/source/gcc-14.2.0-nolibc/aarch64-linux/bin/aarch64-linux-"
board=hi3798mv320-fiberhome-hg680kd-arm64
release="$root/out/release-arm64-$variant"
variant_key=
hdmi=
wifi_probe=
bt=
emmc_hs400=
dvfs=
usb_root=
case "$base_variant" in
    core) ;;
    all-2g|all-2g-hdmi|all-2g-hdmi-wifi-probe|all-2g-hdmi-bt|all-2g-hdmi-bt-emmc-hs400|all-2g-hdmi-bt-dvfs|armbian-usb)
        variant_key="ARCH_HISI USB_XHCI_HI3798MV320 MMC_SDHCI_HI3798MV320 HIX5HD2_GMAC DRM_LIMA COMMON_CLK_HI3798MV320_RESET"
        bbout="$root/out/busybox-arm64-all-2g"
        board=hi3798mv320-fiberhome-hg680kd-arm64-all-2g
        if [[ $base_variant == all-2g-hdmi* || $base_variant == armbian-usb ]]; then
            hdmi=y
            bbout="$root/out/busybox-arm64-all-2g-hdmi"
            variant_key+=" DRM_SIMPLEDRM DRM_FBDEV_EMULATION DRM_CLIENT_DEFAULT_FBDEV FB_DEVICE VT VT_CONSOLE FRAMEBUFFER_CONSOLE"
        fi
        if [[ $base_variant == all-2g-hdmi-wifi-probe ]]; then
            wifi_probe=y
            variant_key+=" GPIOLIB GPIO_PL061 PWRSEQ_SIMPLE CMDLINE_PARTITION MSDOS_PARTITION EXT4_FS"
        fi
        if [[ $base_variant == all-2g-hdmi-bt* || $base_variant == armbian-usb ]]; then
            bt=y
            variant_key+=" GPIOLIB GPIO_PL061 CMDLINE_PARTITION EXT4_FS FW_LOADER BT BT_BREDR BT_LE"
            variant_key+=" SERIAL_DEV_BUS SERIAL_DEV_CTRL_TTYPORT BT_HCIUART_SERDEV BT_HCIUART_3WIRE BT_HCIUART_RTL"
            if [[ $base_variant != armbian-usb ]]; then
                variant_key+=" BT_HCIUART"
            fi
        fi
        if [[ $base_variant == all-2g-hdmi-bt-emmc-hs400 || $base_variant == all-2g-hdmi-bt-dvfs || $base_variant == armbian-usb ]]; then
            emmc_hs400=y
        fi
        if [[ $base_variant == all-2g-hdmi-bt-dvfs || $base_variant == armbian-usb ]]; then
            dvfs=y
            variant_key+=" HI3798MV320_THERMAL REGULATOR_HI3798MV320_CPU CPUFREQ_DT CPU_FREQ_THERMAL DEVFREQ_THERMAL"
            variant_key+=" CPU_FREQ_GOV_SCHEDUTIL CPU_FREQ_GOV_PERFORMANCE CPU_FREQ_GOV_POWERSAVE DEVFREQ_GOV_USERSPACE"
        fi
        if [[ $base_variant == armbian-usb ]]; then
            usb_root=y
        fi
        if [[ $linux_ver == 7.3.* ]]; then
            variant_key+=" USB_STORAGE BLK_DEV_SD VFAT_FS DEVTMPFS DEVTMPFS_MOUNT EFI_PARTITION"
            variant_key+=" F2FS_FS F2FS_FS_XATTR F2FS_FS_POSIX_ACL F2FS_FS_SECURITY F2FS_FS_COMPRESSION F2FS_FS_ZSTD"
            variant_key+=" ZSTD_COMPRESS ZSTD_DECOMPRESS SWAP ZRAM ZRAM_BACKEND_ZSTD ZRAM_DEF_COMP_ZSTD CRYPTO_ZSTD"
        fi
        ;;
    *) echo "Unknown arm64 variant: $variant" >&2; exit 2 ;;
esac
if [[ $linux_ver != 6.18.64 && $variant != "$linux_ver-"* ]]; then
    out="$root/out/arm64-$linux_ver-$base_variant"
    release="$root/out/release-arm64-$linux_ver-$base_variant"
fi

export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP="2025-12-01 00:00:00 UTC"
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1

mkdir -p "$out" "$bbout" "$release"
exec 9>"$root/.build-lock"
flock 9
command -v bc >/dev/null

apply_board_patch() {
    local patchfile=$1
    if ! patch -d "$linux" -p1 --reverse --dry-run --silent <"$patchfile" >/dev/null 2>&1; then
        patch -d "$linux" -p1 --forward <"$patchfile"
    fi
}

patch_0005="$inputs/0005-CLK-hisi-register-Hi3798MV320-reset-controller.patch"
patch_0009="$inputs/0009-CLK-hisi-initialize-reset-controller.patch"
if [[ $linux_ver == 7.3.* ]]; then
    patch_0005="$inputs/0005-CLK-hisi-register-Hi3798MV320-reset-controller-7.3.patch"
    patch_0009="$inputs/0009-CLK-hisi-initialize-reset-controller-7.3.patch"
fi

if [[ $base_variant == core ]]; then
    shopt -s nullglob
    for patchfile in "$inputs"/000[2-5]-*.patch; do
        if patch -d "$linux" -p1 --reverse --dry-run --silent <"$patchfile" >/dev/null 2>&1; then
            patch -d "$linux" -p1 --reverse --batch <"$patchfile"
        fi
    done
    shopt -u nullglob
else
    apply_board_patch "$inputs/0002-USB-xhci-Hi3798MV320-diagnostic.patch"
    install -m644 "$inputs/xhci-hi3798mv320.c" "$linux/drivers/usb/host/"
    apply_board_patch "$inputs/0003-MMC-Hi3798MV320-read-only-diagnostic.patch"
    install -m644 "$inputs/sdhci-hi3798mv320.c" "$linux/drivers/mmc/host/"
    apply_board_patch "$inputs/0004-NET-hix5hd2-try-Hi3798MV320-GMAC.patch"
    apply_board_patch "$patch_0005"
    install -m644 "$inputs/crg-hi3798mv320-reset.c" "$linux/drivers/clk/hisilicon/"
    install -m644 "$inputs/hi3798mv320-fiberhome-hg680kd-arm64.dts" "$linux/arch/arm64/boot/dts/hisilicon/"
fi

# Keep arm32 source tree untouched: install only the arm64 DTS into arch/arm64/boot/dts/hisilicon
install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/$board.dts"
if [[ $hdmi ]]; then
    board+=-hdmi
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/$board.dts"
fi
if [[ $wifi_probe ]]; then
    board+=-wifi-probe
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/$board.dts"
fi
if [[ $bt ]]; then
    apply_board_patch "$inputs/0006-Bluetooth-Realtek-RTL8761BTV-serdev.patch"
    apply_board_patch "$inputs/0007-Bluetooth-btrtl-support-RTL8761BTV-VAT-ROM.patch"
    board+=-bt
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/$board.dts"
fi
if [[ $emmc_hs400 ]]; then
    board+=-emmc-hs400
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/$board.dts"
fi
if [[ $dvfs ]]; then
    apply_board_patch "$inputs/0008-PM-Hi3798MV320-TSENSOR-and-CPU-PWM.patch"
    apply_board_patch "$patch_0009"
    install -m644 "$inputs/hi3798mv320-thermal.c" "$linux/drivers/thermal/"
    install -m644 "$inputs/hi3798mv320-cpu-regulator.c" "$linux/drivers/regulator/"
    board=hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi-bt-dvfs
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/"
    if [[ $usb_root ]]; then
        board+=-usb
        install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/"
    fi
fi
if [[ $vpu_probe ]]; then
    variant_key+=" HI3798MV320_VPU_PROBE"
    apply_board_patch "$inputs/0010-SOC-hisilicon-add-Hi3798MV320-VPU-probe.patch"
    install -m644 "$inputs/hi3798mv320-vpu-probe.c" "$linux/drivers/soc/hisilicon/"
    board=hi3798mv320-fiberhome-hg680kd-arm64-vpu-probe
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/"
fi
if [[ $vpu_h264_poc ]]; then
    variant_key+=" HI3798MV320_VPU_H264_POC"
    apply_board_patch "$inputs/0011-SOC-hisilicon-add-Hi3798MV320-H264-poc.patch"
    install -m644 "$inputs/hi3798mv320-vpu-h264-poc.h" \
        "$inputs/hi3798mv320-vpu-h264-data.h" "$linux/drivers/soc/hisilicon/"
    board=hi3798mv320-fiberhome-hg680kd-arm64-vpu-h264-poc
    install -m644 "$inputs/$board.dts" "$linux/arch/arm64/boot/dts/hisilicon/"
fi
if ! grep -Fq " $board.dtb" "$linux/arch/arm64/boot/dts/hisilicon/Makefile"; then
    printf 'dtb-y += %s.dtb\n' "$board" >> "$linux/arch/arm64/boot/dts/hisilicon/Makefile"
fi

if [[ -z $usb_root ]]; then
# Build AArch64 musl sysroot
if [[ ! -x $sysroot/bin/musl-gcc ]]; then
    mkdir -p "$root/out/musl-arm64-build"
    (
        cd "$root/out/musl-arm64-build"
        CC="${cross}gcc" AR="${cross}ar" RANLIB="${cross}ranlib" "$musl/configure" --target=aarch64-linux \
            --prefix="$sysroot" --disable-shared --enable-wrapper=gcc
        make -j"$jobs"
        make install
    ) >"$root/out/musl-arm64.log" 2>&1
fi
if [[ ! -f $sysroot/include/linux/types.h ]]; then
    make -s -C "$linux" O="$root/out/headers-arm64" ARCH=arm64 CROSS_COMPILE="$cross" headers
    (
        cd "$root/out/headers-arm64/usr/include"
        find . -name '*.h' -print0 | cpio --quiet --null -pdm "$sysroot/include"
    )
fi

# Build AArch64 BusyBox
bb=(make -C "$busybox" O="$bbout" ARCH=arm64 CROSS_COMPILE="$cross" CC="$sysroot/bin/musl-gcc")
busybox_config="$bbout/fragment"
cat "$inputs/busybox.config" "$inputs/busybox-fastfetch.config" "$inputs/busybox-2g.config" >"$busybox_config"
if [[ $base_variant != core ]]; then
    cat "$inputs/busybox-net.config" >>"$busybox_config"
fi
if [[ $hdmi ]]; then
    printf 'CONFIG_CHMOD=y\n' >>"$busybox_config"
fi
"${bb[@]}" allnoconfig >"$root/out/busybox-arm64-config.log"
while IFS='=' read -r key value; do
    [[ $key == CONFIG_* && $value == y ]] || continue
    "$linux/scripts/config" --file "$bbout/.config" --enable "${key#CONFIG_}"
done <"$busybox_config"
"${bb[@]}" oldconfig < <(yes '') >>"$root/out/busybox-arm64-config.log"
if ! "${bb[@]}" -j"$jobs" >"$root/out/busybox-arm64.log" 2>&1; then
    tail -50 "$root/out/busybox-arm64.log"
    exit 1
fi
"${cross}readelf" -hW "$bbout/busybox"

# Cross-compile static AArch64 fastfetch 2.71.0
fastfetch_src="$root/source/fastfetch-2.71.0"
fastfetch_build="$root/out/fastfetch-arm64"
cmake -S "$fastfetch_src" -B "$fastfetch_build" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$sysroot/bin/musl-gcc" \
    -DCMAKE_C_FLAGS="-O2 -static" \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_BUILD_TYPE=Release \
    -DIS_MUSL=ON \
    -DBUILD_FLASHFETCH=OFF \
    -DENABLE_LTO=OFF \
    -DENABLE_VULKAN=OFF \
    -DENABLE_WAYLAND=OFF \
    -DENABLE_XCB_RANDR=OFF \
    -DENABLE_XRANDR=OFF \
    -DENABLE_DRM=OFF \
    -DENABLE_VADRM=OFF \
    -DENABLE_VAX11=OFF \
    -DENABLE_VDPAU=OFF \
    -DENABLE_GIO=OFF \
    -DENABLE_DCONF=OFF \
    -DENABLE_EET=OFF \
    -DENABLE_DBUS=OFF \
    -DENABLE_SQLITE3=OFF \
    -DENABLE_RPM=OFF \
    -DENABLE_EGL=OFF \
    -DENABLE_GLX=OFF \
    -DENABLE_OPENCL=OFF \
    -DENABLE_FREETYPE=OFF \
    -DENABLE_ELF=OFF \
    -DENABLE_IMAGE_LOGO=OFF \
    -DENABLE_IMAGEMAGICK7=OFF \
    -DENABLE_IMAGEMAGICK6=OFF \
    -DENABLE_CHAFA=OFF \
    -DENABLE_ZLIB=OFF \
    -DENABLE_LUA=OFF \
    -DENABLE_LIBZFS=OFF \
    -DSET_TWEAK=OFF >"$root/out/fastfetch-arm64-cmake.log" 2>&1
cmake --build "$fastfetch_build" -j"$jobs" --target fastfetch >"$root/out/fastfetch-arm64-build.log" 2>&1
"${cross}strip" --strip-all "$fastfetch_build/fastfetch" -o "$out/fastfetch"
"${cross}readelf" -hW "$out/fastfetch"

# Create initramfs
os_release="$out/os-release-arm64"
sed "s/6\\.18\\.64/$linux_ver/g" "$inputs/os-release-arm64" >"$os_release"
cc -O2 -o "$out/gen_init_cpio" "$linux/usr/gen_init_cpio.c"
list="$out/initramfs.list"
cat >"$list" <<EOF
dir /bin 0755 0 0
dir /dev 0755 0 0
dir /etc 0755 0 0
dir /etc/fastfetch 0755 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 01777 0 0
nod /dev/console 0600 0 0 c 5 1
file /init $inputs/init-arm64 0755 0 0
file /bin/busybox $bbout/busybox 0755 0 0
file /bin/fastfetch $out/fastfetch 0755 0 0
file /etc/os-release $os_release 0644 0 0
file /etc/hostname $inputs/hostname 0644 0 0
file /etc/passwd $inputs/passwd 0644 0 0
file /etc/fastfetch/config.jsonc $inputs/fastfetch-config.jsonc 0644 0 0
EOF
applets=(cat cttyhack cut dd dmesg echo find free grep hexdump hostname ls mount printf
         readlink reboot setsid sha256sum sh sleep stat umount uname)
if [[ $base_variant != core ]]; then
    applets+=(ifconfig ping wget)
    "$sysroot/bin/musl-gcc" -O2 -static -Wall -Wextra \
        -o "$out/lima-test" "$inputs/lima-test.c"
    "$sysroot/bin/musl-gcc" -O2 -static -Wall -Wextra \
        -o "$out/memtest-2g" "$inputs/memtest-2g.c"
    printf 'file /bin/lima-test %s/lima-test 0755 0 0\n' "$out" >>"$list"
    printf 'file /bin/memtest-2g %s/memtest-2g 0755 0 0\n' "$out" >>"$list"
fi
if [[ $hdmi ]]; then
    applets+=(chmod)
    "$sysroot/bin/musl-gcc" -O2 -static -Wall -Wextra \
        -o "$out/fb-test" "$inputs/fb-test.c"
    printf 'file /bin/fb-test %s/fb-test 0755 0 0\n' "$out" >>"$list"
fi
if [[ $bt ]]; then
    "$sysroot/bin/musl-gcc" -O2 -static -Wall -Wextra \
        -o "$out/bt-test" "$inputs/bt-test.c"
    (cd "$inputs" && sha256sum -c BT-FIRMWARE-SHA256SUMS)
    cat >>"$list" <<EOF
file /bin/bt-test $out/bt-test 0755 0 0
dir /lib 0755 0 0
dir /lib/firmware 0755 0 0
dir /lib/firmware/rtl_bt 0755 0 0
file /lib/firmware/rtl_bt/rtl8761btv_vat_fw.bin $inputs/rtl8761btv_vat_fw 0644 0 0
file /lib/firmware/rtl_bt/rtl8761btv_vat_config.bin $inputs/rtl8761btv_vat_config 0644 0 0
EOF
fi
for applet in "${applets[@]}"; do
    printf 'slink /bin/%s busybox 0777 0 0\n' "$applet" >>"$list"
done
"$out/gen_init_cpio" -t "$SOURCE_DATE_EPOCH" "$list" >"$out/initramfs.cpio"
fi

# Build AArch64 Linux
config="$inputs/arm64.config"
if [[ $base_variant != core ]]; then
    config="$out/arm64.config"
    if [[ $hdmi ]]; then
        sed '/^CONFIG_LOCALVERSION=/d; /^CONFIG_CMDLINE=/d' "$inputs/arm64.config" >"$config"
        if [[ $usb_root ]]; then
            sed '/^CONFIG_LOCALVERSION=/d; /^CONFIG_PANIC_ON_OOPS=/d; /^CONFIG_PANIC_TIMEOUT=/d' "$inputs/arm64-all-2g.config" >>"$config"
        else
            sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm64-all-2g.config" >>"$config"
        fi
        if [[ $wifi_probe || $bt ]]; then
            sed '/^CONFIG_LOCALVERSION=/d; /^CONFIG_CMDLINE=/d' "$inputs/arm64-all-2g-hdmi.config" >>"$config"
            if [[ $dvfs ]]; then
                if [[ $usb_root ]]; then
                    sed '/^CONFIG_LOCALVERSION=/d; /^CONFIG_CMDLINE=/d' "$inputs/arm64-all-2g-hdmi-bt-emmc-hs400.config" >>"$config"
                else
                    sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm64-all-2g-hdmi-bt-emmc-hs400.config" >>"$config"
                fi
            fi
            if [[ $usb_root ]]; then
                sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm64-all-2g-hdmi-bt-dvfs.config" >>"$config"
                cat "$inputs/arm64-7.3.5-f2fs-zram.config" "$inputs/arm64-7.3.5-armbian-usb.config" >>"$config"
            else
                cat "$inputs/arm64-${base_variant}.config" >>"$config"
            fi
        else
            cat "$inputs/arm64-all-2g-hdmi.config" >>"$config"
        fi
        if [[ $linux_ver == 7.3.* && -z $usb_root ]]; then
            cat "$inputs/arm64-7.3.5-f2fs-zram.config" >>"$config"
        fi
    else
        {
            sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm64.config"
            cat "$inputs/arm64-all-2g.config"
        } >"$config"
    fi
fi
kernel=(make -C "$linux" O="$out" ARCH=arm64 CROSS_COMPILE="$cross")
"${kernel[@]}" KCONFIG_ALLCONFIG="$config" allnoconfig
if [[ $vpu_probe ]]; then
    "$linux/scripts/config" --file "$out/.config" --enable HI3798MV320_VPU_PROBE \
        --set-str LOCALVERSION "-hg680kd-arm64-${variant#7.3.5-}"
fi
if [[ $vpu_h264_poc ]]; then
    "$linux/scripts/config" --file "$out/.config" --enable HI3798MV320_VPU_H264_POC
fi
if [[ -z $usb_root ]]; then
    "$linux/scripts/config" --file "$out/.config" --set-str INITRAMFS_SOURCE "$out/initramfs.cpio"
else
    "$linux/scripts/config" --file "$out/.config" --set-str INITRAMFS_SOURCE ""
fi
"${kernel[@]}" olddefconfig
grep -qx "CONFIG_CC_IS_GCC=y" "$out/.config" || { echo "Expected GCC build (CONFIG_CC_IS_GCC=y)" >&2; exit 1; }
for key in ARM64 ARM_GIC_V3 ARM_PSCI_FW ARM_ARCH_TIMER SMP OF \
    SERIAL_AMBA_PL011_CONSOLE SERIAL_EARLYCON CMDLINE_FORCE $variant_key; do
    grep -qx "CONFIG_$key=y" "$out/.config" || { echo "Missing CONFIG_$key" >&2; exit 1; }
done
if [[ -z $usb_root ]]; then
    grep -qx "CONFIG_BLK_DEV_INITRD=y" "$out/.config" || { echo "Missing CONFIG_BLK_DEV_INITRD" >&2; exit 1; }
    "${kernel[@]}" -j"$jobs" Image "hisilicon/$board.dtb"
    install -m644 "$bbout/.config" "$release/busybox.config"
    install -m644 "$out/initramfs.cpio" "$release/"
else
    grep -qx "CONFIG_BT_HCIUART=m" "$out/.config" || { echo "Expected CONFIG_BT_HCIUART=m for usb_root" >&2; exit 1; }
    "${kernel[@]}" -j"$jobs" Image "hisilicon/$board.dtb" modules
    rm -rf "$release/rootfs-mods"
    "${kernel[@]}" INSTALL_MOD_PATH="$release/rootfs-mods" modules_install
    rm -f "$release/rootfs-mods/lib/modules/"*/build "$release/rootfs-mods/lib/modules/"*/source
fi

install -m644 "$out/arch/arm64/boot/Image" "$release/Image"
install -m644 "$out/arch/arm64/boot/dts/hisilicon/$board.dtb" "$release/$board.dtb"
install -m644 "$out/.config" "$release/kernel.config"
install -m644 "$out/System.map" "$out/vmlinux" "$release"
(cd "$release" && sha256sum Image ./*.dtb ./*.config System.map vmlinux $([[ -z $usb_root ]] && echo initramfs.cpio) > SHA256SUMS)
tar -C "$release" -czf "$release.tar.gz" .
ls -lh "$release/Image" "$release/$board.dtb" "$release.tar.gz"
