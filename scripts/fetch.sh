#!/usr/bin/env bash
set -euo pipefail

root=${1:-"$HOME/hg680kd-mainline"}
linux=6.18.64
busybox=1.37.0
musl=1.2.5
toolchain=x86_64-gcc-14.2.0-nolibc-arm-linux-gnueabi
mkdir -p "$root"/{download,source,out,inputs,host-tools}
root=$(realpath "$root")
cd "$root/download"

fetch() {
    local url=$1 file=${1##*/}
    if [[ ! -s $file ]]; then
        curl --fail --location --silent --show-error --retry 3 --connect-timeout 15 \
            --output "$file.part" "$url"
        mv "$file.part" "$file"
    fi
}

fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$linux.tar.xz" &
kernel_pid=$!
fetch "https://busybox.net/downloads/busybox-$busybox.tar.bz2" &
busybox_pid=$!
fetch "https://cdn.kernel.org/pub/tools/crosstool/files/bin/x86_64/14.2.0/$toolchain.tar.xz" &
toolchain_pid=$!
fetch "https://deb.debian.org/debian/pool/main/m/musl/musl_$musl.orig.tar.gz" &
musl_pid=$!

wait "$kernel_pid"
wait "$busybox_pid"
wait "$toolchain_pid"
wait "$musl_pid"

names=(linux-"$linux".tar.xz busybox-"$busybox".tar.bz2 "$toolchain".tar.xz "musl_$musl.orig.tar.gz")
sha256sum --check "$root/inputs/source-SHA256SUMS"

for archive in "${names[@]}"; do
    if [[ ! -f $root/source/.${archive}.extracted ]]; then
        tar -xf "$archive" -C "$root/source"
        touch "$root/source/.${archive}.extracted"
    fi
done
sha256sum "${names[@]}" | tee "$root/out/source-SHA256SUMS"
"$root/source/gcc-14.2.0-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-gcc" --version
