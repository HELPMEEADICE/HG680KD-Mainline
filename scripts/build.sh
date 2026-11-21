#!/usr/bin/env bash
set -euo pipefail

mode=${1:-full}
root=${2:-"$HOME/hg680kd-mainline"}
inputs=${3:-"$root/inputs"}
variant=${4:-core}
if [[ $variant == arm64-* ]]; then
    exec bash "$(dirname "$0")/build_arm64.sh" "$root" "$inputs" "${variant#arm64-}"
fi
jobs=${JOBS:-$(nproc)}
linux="$root/source/linux-6.18.64"
busybox="$root/source/busybox-1.37.0"
musl="$root/source/musl-1.2.5"
sysroot="$root/out/musl"
out="$root/out/arm32"
bbout="$root/out/busybox"
cross="$root/source/gcc-14.2.0-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-"
board=hi3798mv320-fiberhome-hg680kd
release=release-arm32
variant_key=
case "$variant" in
    core) ;;
    usb) variant_key=USB_XHCI_HI3798MV320 ;;
    emmc|sdio) variant_key=MMC_SDHCI_HI3798MV320 ;;
    storage) variant_key="USB_XHCI_HI3798MV320 MMC_SDHCI_HI3798MV320" ;;
    net) variant_key=HIX5HD2_GMAC; bbout="$bbout-net" ;;
    gpu) variant_key="DRM_LIMA COMMON_CLK_HI3798MV320_RESET" ;;
    all) variant_key="USB_XHCI_HI3798MV320 MMC_SDHCI_HI3798MV320 HIX5HD2_GMAC DRM_LIMA COMMON_CLK_HI3798MV320_RESET"; bbout="$bbout-net" ;;
    all-fastfetch) variant_key="USB_XHCI_HI3798MV320 MMC_SDHCI_HI3798MV320 HIX5HD2_GMAC DRM_LIMA COMMON_CLK_HI3798MV320_RESET"; bbout="$bbout-fastfetch" ;;
    all-2g) variant_key="USB_XHCI_HI3798MV320 MMC_SDHCI_HI3798MV320 HIX5HD2_GMAC DRM_LIMA COMMON_CLK_HI3798MV320_RESET HIGHMEM"; bbout="$bbout-2g" ;;
    *) echo "Unknown variant: $variant" >&2; exit 2 ;;
esac
if [[ $variant != core ]]; then
    release="$release-$variant"
    out="$out-$variant"
fi
export PATH="${cross%/*}:$root/host-tools/bin:$PATH"
export LC_ALL=C SOURCE_DATE_EPOCH=1764547200
export KBUILD_BUILD_TIMESTAMP="2025-12-01 00:00:00 UTC"
export KBUILD_BUILD_USER=hg680kd KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1

mkdir -p "$out" "$bbout"
exec 9>"$root/.build-lock"
flock 9
command -v bc >/dev/null
apply_board_patch() {
    local patchfile=$1
    if ! patch -d "$linux" -p1 --reverse --dry-run --silent <"$patchfile" >/dev/null 2>&1; then
        patch -d "$linux" -p1 --forward <"$patchfile"
    fi
}
apply_board_patch "$inputs/0001-ARM-hisi-add-Hi3798MV320-platform.patch"

# Keep the Kconfig and embedded configuration independent of build order.
shopt -s nullglob
for patchfile in "$inputs"/000[2-5]-*.patch; do
    case "$variant:${patchfile##*/}" in
        usb:0002-*|emmc:0003-*|sdio:0003-*|storage:0002-*|storage:0003-*|net:0004-*|net:0005-*|gpu:0005-*|all:000[2-5]-*|all-fastfetch:000[2-5]-*|all-2g:000[2-5]-*) continue ;;
    esac
    if patch -d "$linux" -p1 --reverse --dry-run --silent <"$patchfile" >/dev/null 2>&1; then
        patch -d "$linux" -p1 --reverse --batch <"$patchfile"
    fi
done
shopt -u nullglob

install -m644 "$inputs/$board.dts" "$linux/arch/arm/boot/dts/hisilicon/$board.dts"
config="$inputs/arm32.config"
if [[ $variant == usb || $variant == storage || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
    apply_board_patch "$inputs/0002-USB-xhci-Hi3798MV320-diagnostic.patch"
    install -m644 "$inputs/xhci-hi3798mv320.c" "$linux/drivers/usb/host/"
fi
if [[ $variant == emmc || $variant == sdio || $variant == storage || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
    apply_board_patch "$inputs/0003-MMC-Hi3798MV320-read-only-diagnostic.patch"
    install -m644 "$inputs/sdhci-hi3798mv320.c" "$linux/drivers/mmc/host/"
fi
if [[ $variant == net || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
    apply_board_patch "$inputs/0004-NET-hix5hd2-try-Hi3798MV320-GMAC.patch"
fi
if [[ $variant == net || $variant == gpu || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
    apply_board_patch "$inputs/0005-CLK-hisi-register-Hi3798MV320-reset-controller.patch"
    install -m644 "$inputs/crg-hi3798mv320-reset.c" "$linux/drivers/clk/hisilicon/"
fi
if [[ $variant != core ]]; then
    if [[ $variant == storage ]]; then
        install -m644 "$inputs/$board-emmc.dts" "$inputs/$board-usb.dts" \
            "$linux/arch/arm/boot/dts/hisilicon/"
    elif [[ $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
        install -m644 "$inputs/$board-emmc.dts" "$inputs/$board-usb.dts" \
            "$inputs/$board-net.dts" "$inputs/$board-gpu.dts" \
            "$linux/arch/arm/boot/dts/hisilicon/"
        if [[ $variant == all-2g ]]; then
            install -m644 "$inputs/$board-all.dts" "$linux/arch/arm/boot/dts/hisilicon/"
        fi
    fi
    dts_variant=${variant%-fastfetch}
    board="$board-$dts_variant"
    install -m644 "$inputs/$board.dts" "$linux/arch/arm/boot/dts/hisilicon/$board.dts"
    config="$out/arm32.config"
    if [[ $variant == storage ]]; then
        {
            cat "$inputs/arm32.config"
            printf 'CONFIG_LOCALVERSION="-hg680kd-storage"\n'
            sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm32-emmc.config" "$inputs/arm32-usb.config"
        } >"$config"
    elif [[ $variant == all || $variant == all-fastfetch ]]; then
        {
            cat "$inputs/arm32.config"
            printf 'CONFIG_LOCALVERSION="-hg680kd-%s"\n' "$variant"
            sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm32-emmc.config" "$inputs/arm32-usb.config" \
                "$inputs/arm32-net.config" "$inputs/arm32-gpu.config"
        } >"$config"
    elif [[ $variant == all-2g ]]; then
        {
            cat "$inputs/arm32.config"
            printf 'CONFIG_LOCALVERSION="-hg680kd-all-2g"\n'
            sed '/^CONFIG_LOCALVERSION=/d' "$inputs/arm32-emmc.config" "$inputs/arm32-usb.config" \
                "$inputs/arm32-net.config" "$inputs/arm32-gpu.config" "$inputs/arm32-2g.config"
        } >"$config"
    else
        cat "$inputs/arm32.config" "$inputs/arm32-$variant.config" >"$config"
    fi
fi
kernel=(make -C "$linux" O="$out" ARCH=arm CROSS_COMPILE="$cross")

if [[ $mode == full ]]; then
    if [[ ! -x $sysroot/bin/musl-gcc ]]; then
        mkdir -p "$root/out/musl-build"
        (
            cd "$root/out/musl-build"
            CC="${cross}gcc" "$musl/configure" --target=arm-linux-gnueabi \
                --prefix="$sysroot" --disable-shared --enable-wrapper=gcc
            make -j"$jobs"
            make install
        ) >"$root/out/musl.log" 2>&1
    fi
    if [[ ! -f $sysroot/include/linux/types.h ]]; then
        make -s -C "$linux" O="$root/out/headers" ARCH=arm CROSS_COMPILE="$cross" headers
        (
            cd "$root/out/headers/usr/include"
            find . -name '*.h' -print0 | cpio --quiet --null -pdm "$sysroot/include"
        )
    fi

    bb=(make -C "$busybox" O="$bbout" ARCH=arm CROSS_COMPILE="$cross" CC="$sysroot/bin/musl-gcc")
    busybox_config="$bbout/fragment"
    cp "$inputs/busybox.config" "$busybox_config"
    if [[ $variant == net || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
        cat "$inputs/busybox-net.config" >>"$busybox_config"
    fi
    if [[ $variant == all-fastfetch || $variant == all-2g ]]; then
        cat "$inputs/busybox-fastfetch.config" >>"$busybox_config"
    fi
    if [[ $variant == all-2g ]]; then
        cat "$inputs/busybox-2g.config" >>"$busybox_config"
    fi
    "${bb[@]}" allnoconfig >"$root/out/busybox-config.log"
    while IFS='=' read -r key value; do
        [[ $key == CONFIG_* && $value == y ]] || continue
        "$linux/scripts/config" --file "$bbout/.config" --enable "${key#CONFIG_}"
    done <"$busybox_config"
    "${bb[@]}" oldconfig < <(yes '') >>"$root/out/busybox-config.log"
    while IFS='=' read -r key value; do
        [[ $key == CONFIG_* && $value == y ]] || continue
        grep -qx "$key=y" "$bbout/.config" || { echo "Missing BusyBox $key" >&2; exit 1; }
    done <"$busybox_config"
    if ! "${bb[@]}" -j"$jobs" >"$root/out/busybox.log" 2>&1; then
        tail -50 "$root/out/busybox.log"
        exit 1
    fi
    "${cross}readelf" -hW "$bbout/busybox"

    init_src="$inputs/init"
    if [[ $variant == all-fastfetch || $variant == all-2g ]]; then
        init_src="$inputs/init-fastfetch"
    fi
    cc -O2 -o "$out/gen_init_cpio" "$linux/usr/gen_init_cpio.c"
    list="$out/initramfs.list"
    cat >"$list" <<EOF
dir /bin 0755 0 0
dir /dev 0755 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 01777 0 0
nod /dev/console 0600 0 0 c 5 1
file /init $init_src 0755 0 0
file /bin/busybox $bbout/busybox 0755 0 0
EOF
    applets=(cat cttyhack cut dd dmesg echo find grep hexdump ls mount printf
             readlink reboot setsid sha256sum sh sleep stat umount uname)
    if [[ $variant == net || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
        applets+=(ifconfig ping wget)
    fi
    if [[ $variant == all-fastfetch || $variant == all-2g ]]; then
        applets+=(hostname)
    fi
    if [[ $variant == all-2g ]]; then
        applets+=(free)
    fi
    if [[ $variant == gpu || $variant == all || $variant == all-fastfetch || $variant == all-2g ]]; then
        "$sysroot/bin/musl-gcc" -O2 -static -Wall -Wextra \
            -o "$out/lima-test" "$inputs/lima-test.c"
        printf 'file /bin/lima-test %s/lima-test 0755 0 0\n' "$out" >>"$list"
    fi
    if [[ $variant == all-2g ]]; then
        "$sysroot/bin/musl-gcc" -O2 -static -Wall -Wextra \
            -o "$out/memtest-2g" "$inputs/memtest-2g.c"
        printf 'file /bin/memtest-2g %s/memtest-2g 0755 0 0\n' "$out" >>"$list"
    fi
    if [[ $variant == all-fastfetch || $variant == all-2g ]]; then
        fastfetch_src="$root/source/fastfetch-2.71.0"
        fastfetch_build="$root/out/fastfetch"
        cmake -S "$fastfetch_src" -B "$fastfetch_build" \
            -DCMAKE_SYSTEM_NAME=Linux \
            -DCMAKE_SYSTEM_PROCESSOR=armv7l \
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
            -DSET_TWEAK=OFF >"$root/out/fastfetch-cmake.log" 2>&1
        cmake --build "$fastfetch_build" -j"$jobs" --target fastfetch >"$root/out/fastfetch-build.log" 2>&1
        "${cross}strip" --strip-all "$fastfetch_build/fastfetch" -o "$out/fastfetch"
        "${cross}readelf" -hW "$out/fastfetch"
        cat >>"$list" <<EOF
dir /etc 0755 0 0
dir /etc/fastfetch 0755 0 0
file /etc/os-release $inputs/os-release 0644 0 0
file /etc/hostname $inputs/hostname 0644 0 0
file /etc/passwd $inputs/passwd 0644 0 0
file /etc/fastfetch/config.jsonc $inputs/fastfetch-config.jsonc 0644 0 0
file /bin/fastfetch $out/fastfetch 0755 0 0
EOF
    fi
    for applet in "${applets[@]}"; do
        printf 'slink /bin/%s busybox 0777 0 0\n' "$applet" >>"$list"
    done
    "$out/gen_init_cpio" -t "$SOURCE_DATE_EPOCH" "$list" >"$out/initramfs.cpio"
fi

"${kernel[@]}" KCONFIG_ALLCONFIG="$config" allnoconfig
if [[ $mode == full ]]; then
    "$linux/scripts/config" --file "$out/.config" --set-str INITRAMFS_SOURCE "$out/initramfs.cpio"
    "${kernel[@]}" olddefconfig
fi
for key in ARCH_HI3798MV320 ARM_GIC_V3 ARM_PSCI ARM_ARCH_TIMER SMP OF COMPAT_32BIT_TIME \
    ARM_APPENDED_DTB SERIAL_AMBA_PL011_CONSOLE BLK_DEV_INITRD CMDLINE_FORCE $variant_key; do
    grep -qx "CONFIG_$key=y" "$out/.config" || { echo "Missing CONFIG_$key" >&2; exit 1; }
done
"${kernel[@]}" -j"$jobs" zImage "hisilicon/$board.dtb"

if [[ $mode == full ]]; then
    release="$root/out/$release"
    mkdir -p "$release"
    install -m644 "$out/arch/arm/boot/zImage" "$release/zImage"
    install -m644 "$out/arch/arm/boot/dts/hisilicon/$board.dtb" "$release/$board.dtb"
    install -m644 "$out/.config" "$release/kernel.config"
    install -m644 "$bbout/.config" "$release/busybox.config"
    install -m644 "$out/System.map" "$out/vmlinux" "$out/initramfs.cpio" "$release"
    install -m644 "$root/out/source-SHA256SUMS" "$release/source-SHA256SUMS"
    (cd "$release" && sha256sum zImage ./*.dtb ./*.config System.map vmlinux initramfs.cpio \
        source-SHA256SUMS > SHA256SUMS)
    tar -C "$release" -czf "$release.tar.gz" .
fi
