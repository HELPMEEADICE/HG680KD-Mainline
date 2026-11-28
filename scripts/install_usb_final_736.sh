#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C

disk=/dev/sda
boot_part=/dev/sda1
root_part=/dev/sda3
boot=/boot/boot.img
backup=$boot.bak
manifest=/boot/BOOT-SHA256SUMS
player_dir=/opt/vdp-player
runtime=/opt/hg680kd-mpv-native-drain
bin=/usr/local/bin
size=15245384
sector_start=4136
sector_count=29777
tools=(mpv ffmpeg-v4l2request ffprobe-v4l2request)

assert_usb() {
    test "$(id -u)" = 0
    test "$(uname -r)" = 7.3.6-hg680kd-arm64-usb-final
    test "$(lsblk -dn -o TRAN "$disk")" = usb
    test "$(lsblk -dn -o PKNAME "$boot_part")" = sda
    test "$(lsblk -dn -o PKNAME "$root_part")" = sda
    test "$(lsblk -dn -o PARTUUID "$boot_part")" = caf68121-6e1d-421d-9f34-15cb4511505a
    test "$(lsblk -dn -o PARTUUID "$root_part")" = 228149da-74d1-41f8-99ad-9d1e122d7964
    test "$(lsblk -dn -o PARTLABEL "$boot_part")" = HG680KD_BOOT
    test "$(lsblk -dn -o PARTLABEL "$root_part")" = HG680KD_ROOT
    test "$(cat /sys/block/sda/size)" = 60125184
    test "$(cat /sys/block/sda/queue/logical_block_size)" = 512
    test "$(cat /sys/block/sda/sda1/start)" = 2048
    test "$((sector_start + sector_count))" -le "$((2048 + $(cat /sys/block/sda/sda1/size)))"
    if compgen -G '/dev/mmcblk*' >/dev/null; then return 1; fi
    for dir in / /opt "$player_dir" "$bin" /etc; do
        test "$(realpath -e "$dir")" = "$dir"
        test "$(findmnt -nro SOURCE,FSTYPE -T "$dir")" = "$root_part f2fs"
    done
}

check_boot_mount() {
    test "$(findmnt -nro SOURCE,FSTYPE -M /boot)" = "$boot_part vfat"
}

read_sectors() {
    dd if="$disk" of="$1" bs=512 skip="$sector_start" count="$sector_count" iflag=direct status=none
    test "$(stat -c %s "$1")" = "$((512 * sector_count))"
}

publish() {
    local staged
    staged=$(mktemp "$(dirname "$3")/.hg680kd.XXXXXXXX")
    install -m "$1" "$2" "$staged"
    mv -fT "$staged" "$3"
}

assert_usb
test "$(findmnt -nro FSTYPE -T "$PWD")" = tmpfs
check_boot_mount
exec 9>/run/lock/hg680kd-usb-final.lock
flock -n 9
mkdir before
sha256sum -c INPUT-SHA256SUMS
test ! -e "$runtime" && test ! -L "$runtime"
for name in "${tools[@]}"; do
    test ! -e "$bin/$name" && test ! -L "$bin/$name"
done
for path in "$boot" "$manifest" "$player_dir/vdp-player" "$player_dir/SHA256SUMS"; do
    test -f "$path" && test ! -L "$path"
done
test "$(stat -c %s "$boot")" = "$size"
test "$(stat -c %s boot.img)" = "$size"
cp "$boot" before/boot.img
cp "$manifest" before/BOOT-SHA256SUMS
cp -p "$player_dir/vdp-player" "$player_dir/SHA256SUMS" before/
(cd before; grep '  boot\.img$' BOOT-SHA256SUMS | sha256sum -c -)
(cd "$player_dir"; sha256sum -c SHA256SUMS)
(cd "$player_dir"; grep -v '  vdp-player$' "$OLDPWD/PLAYER-SHA256SUMS" | sha256sum -c -)
old=$(sha256sum before/boot.img | cut -d' ' -f1)
new=$(sha256sum boot.img | cut -d' ' -f1)
grep -Fx "$(sha256sum vdp-player)" PLAYER-SHA256SUMS

boot_changed=0
player_changed=0
runtime_changed=0
published=()
rollback() {
    local status=$?
    trap - ERR
    assert_usb
    if [[ $boot_changed = 1 ]]; then
        if ! mountpoint -q /boot; then mount -t vfat "$boot_part" /boot; fi
        check_boot_mount
        mount -o remount,rw /boot
        dd if=before/boot.img of="$boot" bs=1M conv=notrunc,fsync status=none
        cat before/BOOT-SHA256SUMS > "$manifest"
        cmp before/boot.img "$boot"
        sync -f /boot
    fi
    if mountpoint -q /boot; then check_boot_mount; umount /boot; fi
    if [[ $boot_changed = 1 ]]; then
        read_sectors raw.restored
        cmp raw.before raw.restored
    fi
    if [[ $player_changed = 1 ]]; then
        publish 755 before/vdp-player "$player_dir/vdp-player"
        publish 644 before/SHA256SUMS "$player_dir/SHA256SUMS"
        cmp before/vdp-player "$player_dir/vdp-player"
    fi
    for name in "${published[@]}"; do rm -f -- "$bin/$name"; done
    if [[ $runtime_changed = 1 ]]; then mv -T "$runtime" "$runtime_stage"; fi
    sync -f /
    echo USB_ROLLBACK_VERIFIED
    exit "$status"
}
trap rollback ERR

# The accepted MPV and FFmpeg are musl builds. Keep their loader and graphics
# and codec libraries together under /opt so Armbian's glibc tools keep working.
runtime_stage=$(mktemp -d /opt/.hg680kd-mpv.XXXXXXXX)
chmod 755 "$runtime_stage"
tar --no-same-owner -xzf mpv-runtime.tar.gz -C "$runtime_stage"
rm -- mpv-runtime.tar.gz
install -m 755 mpv-native-drain "$runtime_stage/usr/bin/mpv"
mv -T "$runtime_stage" "$runtime"
runtime_changed=1
loader=$runtime/lib/ld-musl-aarch64.so.1
libs=$runtime/usr/lib:$runtime/lib
for name in mpv ffmpeg ffprobe; do
    "$loader" --list --library-path "$libs" "$runtime/usr/bin/$name" | tee "$name.ldd"
    grep -F "$runtime/usr/lib/libavcodec.so.62" "$name.ldd"
    grep -F "$runtime/usr/lib/libavutil.so.60" "$name.ldd"
    if grep -Ei 'not found|error loading|error relocating' "$name.ldd"; then false; fi
done
cmp mpv-native-drain "$runtime/usr/bin/mpv"
for name in "${tools[@]}"; do
    cat > "$name.launcher" <<'EOF'
#!/bin/sh
set -eu
runtime=/opt/hg680kd-mpv-native-drain
loader=$runtime/lib/ld-musl-aarch64.so.1
libs=$runtime/usr/lib:$runtime/lib
name=${0##*/}
if [ "$name" = mpv ]; then
    export GBM_BACKENDS_PATH="$runtime/usr/lib/gbm"
    export LIBGL_DRIVERS_PATH="$runtime/usr/lib/dri"
    export ALSA_CONFIG_DIR="$runtime/usr/share/alsa"
    export ALSA_CONFIG_PATH="$ALSA_CONFIG_DIR/alsa.conf"
    set -- --hwdec=v4l2request --gpu-hwdec-interop=drmprime-overlay --drm-v4l2request-mv320-tiled "$@"
fi
exec "$loader" --library-path "$libs" "$runtime/usr/bin/${name%-v4l2request}" "$@"
EOF
    published+=("$name")
    publish 755 "$name.launcher" "$bin/$name"
done
ldconfig
"$bin/mpv" --version
"$bin/mpv" --no-config --hwdec=help | grep -w v4l2request
"$bin/ffmpeg-v4l2request" -hwaccels | grep -Fx v4l2request
"$bin/ffprobe-v4l2request" -version

lsblk -dn -o PATH,TRAN,MODEL,SIZE "$disk"
lsblk -dn -o PATH,PARTLABEL,PARTUUID "$boot_part" "$root_part"
umount /boot
read_sectors raw.before
cmp -n "$size" before/boot.img raw.before
mount -t vfat "$boot_part" /boot
check_boot_mount
cmp before/boot.img "$boot"
if [[ -e $backup ]]; then
    test -f "$backup" && test ! -L "$backup"
    prior=$(sha256sum "$backup" | cut -d' ' -f1)
    if [[ $prior != "$old" ]]; then
        archived="$backup.$prior"
        if [[ ! -e $archived ]]; then cp "$backup" "$archived"; fi
        cmp "$backup" "$archived"
    fi
fi
publish 644 before/boot.img "$backup"
cmp before/boot.img "$backup"
sync -f /boot
boot_changed=1
dd if=boot.img of="$boot" bs=1M conv=notrunc,fsync status=none
sed "s/^$old  boot\.img$/$new  boot.img/" before/BOOT-SHA256SUMS > "$manifest"
sync -f /boot
umount /boot

read_sectors raw.after
cmp -n "$size" boot.img raw.after
cmp <(tail -c "$((512 * sector_count - size))" raw.before) \
    <(tail -c "$((512 * sector_count - size))" raw.after)
raw_sha=$(head -c "$size" raw.after | sha256sum | cut -d' ' -f1)
test "$raw_sha" = "$new"
printf 'Raw LBA %d count %d payload SHA256 %s\n' "$sector_start" "$sector_count" "$raw_sha"
sha256sum raw.after
mount -t vfat -o ro "$boot_part" /boot
check_boot_mount
cmp boot.img "$boot"
cmp before/boot.img "$backup"
grep -Fx "$new  boot.img" "$manifest"
sha256sum "$boot" "$backup"
umount /boot

player_changed=1
publish 755 vdp-player "$player_dir/vdp-player"
publish 644 PLAYER-SHA256SUMS "$player_dir/SHA256SUMS"
(cd "$player_dir"; sha256sum -c SHA256SUMS)
cmp vdp-player "$player_dir/vdp-player"
test "$(stat -c %a "$player_dir/vdp-player")" = 755
sha256sum "$player_dir/vdp-player" "$runtime/usr/bin/mpv" "$bin/"{mpv,ffmpeg-v4l2request,ffprobe-v4l2request}
sync
! mountpoint -q /boot || false
trap - ERR
echo USB_BOOT_AND_MEDIA_VERIFIED
