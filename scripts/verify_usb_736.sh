#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
case_name=$1
boot_sum=$2
player_sum=$3
config_sum=$4
display=/sys/bus/platform/devices/f8cc0000.display-controller/scanout
cma=/sys/kernel/debug/cma/reserved/used
crg=/sys/kernel/debug/clk/venc/crg_registers
dmabuf=/sys/kernel/debug/dma_buf/bufinfo
player=/opt/vdp-player/run

test "$(findmnt -nro FSTYPE -T "$PWD")" = tmpfs
test "$(findmnt -nro SOURCE,FSTYPE -M /)" = '/dev/sda3 f2fs'
test "$(lsblk -dn -o TRAN /dev/sda)" = usb
release=7.3.6-hg680kd-arm64-usb
final=0
case "$case_name" in
    cold-final|cold-final-media) release+=-final; final=1 ;;
esac
test "$(uname -r)" = "$release"
test ! -e /dev/mmcblk0
case "$case_name" in
    cold|cold-final|cold-final-media) test "$(findmnt -nro SOURCE,FSTYPE -M /boot)" = '/dev/sda1 vfat'
          printf '%s  /boot/boot.img\n' "$boot_sum" | sha256sum -c - ;;
    preflight) ! mountpoint -q /boot ;;
    *) exit 1 ;;
esac
grep -F 'root=PARTLABEL=HG680KD_ROOT rootfstype=f2fs' /proc/cmdline
grep -qw cma=384M /proc/cmdline
grep -E '^CmaTotal:[[:space:]]+393216 kB' /proc/meminfo
zcat /proc/config.gz > kernel.config
printf '%s  kernel.config\n' "$config_sum" | sha256sum -c -
grep -F 'CONFIG_KVM=y' kernel.config
dmesg | grep -Ei 'started at EL[12]|HYP mode|kvm|firmware HD0|fbcon:|Switching to.*frame buffer'
printf '%s  /opt/vdp-player/vdp-player\n' "$player_sum" | sha256sum -c -
test "$(stat -c %a /opt/vdp-player/vdp-player)" = 755
for device in /dev/fb0 /dev/dri/card0 /dev/dri/renderD128 /dev/video{0,1,2} /dev/media0 /dev/snd/pcmC0D0p; do
    test -c "$device"
    ls -l "$device"
done
for node in /sys/class/video4linux/video{0,1,2}; do printf '%s ' "$node"; cat "$node/name"; done
cat /sys/class/graphics/fb0/name
grep -Fx connected /sys/class/drm/card0-HDMI-A-1/status
readlink -f /sys/class/drm/card0/device/driver /sys/class/drm/renderD128/device/driver
sha256sum -c TEST-SHA256SUMS
chmod 755 lima-test jpgd-test vdec-mpeg-test ffmpeg venc-ioctl-smoke
if [[ $final = 1 ]]; then
    chmod 755 modetest drm-vblank-measure
    source ./verify_usb_hdmi_736.sh
    native_inventory "${5:?MPV SHA256 required}"
    cat /proc/sys/kernel/random/boot_id
fi

irq() {
    awk -v irq="$1" '$6 == "GICv3" && $7 == irq { print $2 + $3 + $4 + $5; seen++ }
        END { if (seen != 1) exit 1 }' /proc/interrupts
}

snapshot() {
    printf '%s\n' "$1"
    grep -E 'GICv3 +(116|122|126|129|133|169|181) ' /proc/interrupts
    grep -E '^Cma(Total|Free):' /proc/meminfo
    printf 'CMA reserved/used='; cat "$cma"
    cat "$crg"
    grep -E '^Total ' "$dmabuf"
    grep -E '^(vblank_irqs|fetch_underflow)' "$display"
    test "$(irq 181)" = 0
}

test "$(systemctl is-active lightdm)" = active
export DISPLAY=:0 XAUTHORITY=/run/lightdm/root/:0
pgrep -xa xfce4-session
xrandr --current
glxinfo -B | tee glxinfo.log
grep -F 'Accelerated: yes' glxinfo.log
grep -F 'OpenGL renderer string: Mali450' glxinfo.log
es2_info | tee es2.log
grep -Ei 'EGL_VENDOR:.*Mesa' es2.log
grep -E 'GL_RENDERER:.*Mali450' es2.log
timeout -k 5 40 glmark2 --size 1920x1080 -b build:duration=3 -b texture:duration=3 -b shading:duration=3 | tee glmark2.log
grep -Eq 'GL_RENDERER:[[:space:]]+Mali450[[:space:]]*$' glmark2.log
grep -Eq 'glmark2 Score: [1-9]' glmark2.log
timeout -k 5 30 ./lima-test -a -n 32 | tee lima.log
grep -q 'num_pp=2' lima.log
test "$(grep -c '4096/4096 pixels matched' lima.log)" = 32
test "$(grep -Ec '^Job .* cores=1$' lima.log)" = 16
test "$(grep -Ec '^Job .* cores=2$' lima.log)" = 16
grep -F 'LIMA GP+PP OFFSCREEN RENDER TEST: PASS' lima.log

systemctl stop lightdm
trap 'systemctl start lightdm' EXIT
if [[ $final = 1 ]]; then
    trap 'native_cleanup; systemctl start lightdm' EXIT
    cp "$kms_state" console.before
fi
if [[ $case_name = cold-final ]]; then
    native_hdmi_and_pcm
fi
for attempt in $(seq 1 10); do
    if grep -Eq '^Total 0 objects, 0 bytes' "$dmabuf"; then break; fi
    sleep 1
done
grep -E '^Total 0 objects, 0 bytes' "$dmabuf"
if [[ $case_name = cold-final ]]; then
    mpv_cma=$(cat "$cma")
    snapshot BEFORE_MPV
    native_mpv
    snapshot AFTER_MPV
    test "$(cat "$cma")" -le "$mpv_cma"
    grep -E '^Total 0 objects, 0 bytes' "$dmabuf"
    native_cleanup
    sleep 1
    cp "$kms_state" console.after-mpv
    grep -F 'mode: "1920x1080": 60 ' console.after-mpv
    check_scanout
fi
baseline_cma=$(cat "$cma")
baseline_free=$(awk '/^CmaFree:/ {print $2}' /proc/meminfo)
baseline_crg=$(cat "$crg")
snapshot BEFORE_MEDIA

av_before=$(irq 169)
audio_before=$(irq 116)
timeout -k 4 40 "$player" --osd main10-av.mkv | tee hevc.log
grep -Eq 'V0 source=3840x2160 .*fourcc=H0TV modifier=0xfe00000000000320' hevc.log
grep -F 'blend=Coverage' hevc.log
grep -E '^G0 progress flips=[1-9]' hevc.log
grep -E '^PLAY decoded=1080 displayed=1080 dropped=0 .*eos=1 missed_vblanks=0 fps=60\.000' hevc.log
grep -F 'ALSA played=864000 frames rate=48000 xruns=0' hevc.log
test "$(( $(irq 169) - av_before ))" = 1080
test "$(( $(irq 116) - audio_before ))" -gt 0

h264_before=$(irq 169)
timeout -k 4 40 "$player" h264-1080p60.mp4 | tee h264.log
grep -E '^PLAY decoded=1080 displayed=1080 dropped=0 .*eos=1 missed_vblanks=0 fps=60\.' h264.log
test "$(( $(irq 169) - h264_before ))" = 1080

for codec in 1 2; do
    vdh_before=$(irq 169)
    timeout -k 3 25 ./vdec-mpeg-test /dev/video0 /dev/media0 "mpeg$codec-1080p.requests" | tee "mpeg$codec.log"
    grep -E "^MPEG$codec decoded=120 fps=" "mpeg$codec.log"
    test "$(( $(irq 169) - vdh_before ))" = 120
done

jpg_before=$(irq 129)
timeout -k 3 15 ./jpgd-test /dev/video1 1920 1080 1080p.jpg | tee jpgd.log
grep -E '^device=/dev/video1 driver=hi3798-jpgd visible=1920x1080' jpgd.log
test "$(( $(irq 129) - jpg_before ))" = 1

venc_before=$(irq 133)
timeout -k 3 40 ./ffmpeg -nostdin -y -v debug -xerror -nostats -benchmark \
    -f rawvideo -pixel_format nv12 -video_size 1920x1080 -framerate 30 -i venc-regression.nv12 \
    -an -pix_fmt nv12 -c:v h264_v4l2m2m -g 12 -profile:v 578 venc-usb.h264 > ffmpeg.log 2>&1 || { cat ffmpeg.log; exit 1; }
grep -E 'Using device /dev/video2|12 frames encoded' ffmpeg.log
grep -F 'Using device /dev/video2' ffmpeg.log
grep -F '12 frames encoded' ffmpeg.log
test "$(( $(irq 133) - venc_before ))" = 12
./venc-ioctl-smoke --log-status
dmesg | grep -F 'SMMU faults=' | tail -1 | tee venc-status.log
grep -F 'SMMU faults=0 quarantined=0' venc-status.log
sha256sum venc-usb.h264

snapshot AFTER_MEDIA
if [[ $final = 1 ]]; then
    test "$(cat "$cma")" -le "$baseline_cma"
else
    test "$(cat "$cma")" = "$baseline_cma"
fi
test "$(( baseline_free - $(awk '/^CmaFree:/ {print $2}' /proc/meminfo) ))" -le 4096
test "$(cat "$crg")" = "$baseline_crg"
grep -E '^Total 0 objects, 0 bytes' "$dmabuf"
grep -E '^vblank_irqs .*underflow_irqs 0$' "$display"
grep -Fx 'fetch_underflow 00000000' "$display"

if [[ $final = 1 ]]; then
    native_cleanup
    sleep 1
    cp "$kms_state" console.after
    grep -F 'mode: "1920x1080": 60 ' console.after
    check_scanout
fi
systemctl start lightdm
trap - EXIT
test "$(systemctl is-active lightdm)" = active
test "$(irq 181)" = 0
if [[ $final = 1 ]]; then
    for attempt in {1..20}; do
        if xrandr --current > desktop-restored.log 2>&1; then break; fi
        sleep .5
    done
    grep -E ' connected( primary)? [0-9]+x[0-9]+\+' desktop-restored.log
    check_scanout
    dmesg --color=never > dmesg.log
fi
dmesg --color=never -l warn,err,crit,alert,emerg
if dmesg --color=never | grep -Ei 'lima.*(error|time ?out)|SMMU (fault|error)([^s]|$)|CRG restore failed|USB disconnect|I/O error|Kernel panic|external abort|Oops:|BUG:'; then exit 1; fi
if [[ $case_name = cold-final-media ]]; then
    echo USB_FINAL_MEDIA_ACCEPTANCE_OK
else
    echo USB_FULL_ACCEPTANCE_OK
fi
