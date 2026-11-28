#!/usr/bin/env bash
# Sourced by verify_usb_736.sh for native HDMI, PCM and MPV checks.

kms_state=/sys/kernel/debug/dri/0/state
connector_dir=/sys/class/drm/card0-HDMI-A-1
pcm_status=/proc/asound/card0/pcm0p/sub0/status
governor_file=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
runtime=/opt/hg680kd-mpv-native-drain
musl=("$runtime/lib/ld-musl-aarch64.so.1" --library-path "$runtime/usr/lib:$runtime/lib")
mode_pid=
tone_pid=
old_governor=

native_cleanup() {
    if [[ -n $mode_pid ]]; then
        kill "$mode_pid" 2>/dev/null || :
        wait "$mode_pid" 2>/dev/null || :
        mode_pid=
    fi
    if [[ -n $tone_pid ]]; then
        kill "$tone_pid" 2>/dev/null || :
        wait "$tone_pid" 2>/dev/null || :
        tone_pid=
    fi
    if [[ -n $old_governor ]]; then
        echo "$old_governor" > "$governor_file"
        old_governor=
    fi
}

native_inventory() {
    printf '%s  %s\n' "$1" "$runtime/usr/bin/mpv" | sha256sum -c -
    test "$(command -v mpv)" = /usr/local/bin/mpv
    "$runtime/lib/ld-musl-aarch64.so.1" --list --library-path "$runtime/usr/lib:$runtime/lib" \
        "$runtime/usr/bin/mpv" > mpv.ldd
    grep -F "$runtime/usr/lib/libavcodec.so.62" mpv.ldd
    if grep -Ei 'not found|error loading|error relocating' mpv.ldd; then return 1; fi
    "${musl[@]}" ./modetest -M hi3798mv320-vdp -a -c -p > drm-inventory.txt
    cat "$connector_dir/status" "$connector_dir/modes"
    grep -Fx connected "$connector_dir/status"
    cat "$connector_dir/edid" > edid.bin
    test "$(wc -c < edid.bin)" = 256
    printf '%s  edid.bin\n' b2538071e7ee7a08a2da912419313326b955233d2204894227c984e6c11a060f | sha256sum -c -
    grep -E 'GICv3 +120 +Level +hi3798mv320-hpd' /proc/interrupts
    grep -E '1920x1080 60\.00|3840x2160 (30|60)\.00' drm-inventory.txt
    connector_id=$(awk '$3=="connected" && $4=="HDMI-A-1" {print $1}' drm-inventory.txt)
    crtc_id=$(sed -nE 's/^crtc\[([0-9]+)\]: HD0$/\1/p' "$kms_state")
    primary_id=$(sed -nE 's/^plane\[([0-9]+)\]: G0$/\1/p' "$kms_state")
    [[ $connector_id =~ ^[0-9]+$ && $crtc_id =~ ^[0-9]+$ && $primary_id =~ ^[0-9]+$ ]]
}

check_scanout() {
    grep -E '^vblank_irqs .*underflow_irqs 0$' "$display"
    grep -Fx 'fetch_underflow 00000000' "$display"
    test "$(irq 181)" = 0
}

native_mode() {
    local label=$1 width=$2 height=$3 hz=$4 vic=$5 scale=1 previous=$mode_pid tries
    if [[ $width = 3840 && $hz = 60 ]]; then scale=2; fi
    mkfifo "$label.fifo"
    (
        exec 3<>"$label.fifo"
        exec "${musl[@]}" ./modetest -M hi3798mv320-vdp -a -d -s "$connector_id@$crtc_id:${width}x${height}-$hz" \
            -P "$primary_id@$crtc_id:$((width/scale))x$((height/scale))*$scale@XR24" \
            -F smpte,smpte <&3 >"$label.modetest" 2>&1
    ) &
    mode_pid=$!
    for tries in {1..40}; do
        kill -0 "$mode_pid"
        if grep -Fq "mode: \"${width}x${height}\": $hz " "$kms_state"; then break; fi
        sleep .1
    done
    if [[ -n $previous ]]; then
        kill "$previous"
        wait "$previous" 2>/dev/null || :
    fi
    grep -F "mode: \"${width}x${height}\": $hz " "$kms_state"
    cat "$kms_state" > "$label.state"
    # Let the TX PLL and sink lock before measuring steady-state vblank.
    sleep 1
    ./drm-vblank-measure | tee "$label.vblank"
    awk -F 'hz=' -v rate="$hz" '$2+0 > rate-.02 && $2+0 < rate+.02 {ok=1} END {exit !ok}' "$label.vblank"
    printf 'HDMI_MODE %s %s %s %s %s\n' "$label" "$width" "$height" "$hz" "$vic"
    sleep 1.5
    grep -Fx 'state: RUNNING' "$pcm_status"
    test "$(awk '$1=="owner_pid" {print $3}' "$pcm_status")" = "$pcm_owner"
    check_scanout
}

native_hdmi_and_pcm() {
    local attempt pcm_owner=
    "$player" --tone 32 > continuous-pcm.log 2>&1 &
    tone_pid=$!
    for attempt in {1..40}; do
        if grep -Fxq 'state: RUNNING' "$pcm_status"; then
            pcm_owner=$(awk '$1=="owner_pid" {print $3}' "$pcm_status")
            break
        fi
        sleep .1
    done
    test -n "$pcm_owner"
    native_mode 1080p60-first 1920 1080 60 16
    native_mode 2160p30 3840 2160 30 95
    native_mode 2160p60-first 3840 2160 60 97
    native_mode 1080p60-return 1920 1080 60 16
    native_mode 2160p60-return 3840 2160 60 97
    wait "$tone_pid"
    tone_pid=
    grep -F 'ALSA played=1536000 frames rate=48000 xruns=0' continuous-pcm.log
}

native_mpv() {
    local attempt rc before_irq passed=0
    awk '/^plane\[|^[[:space:]]+fb=|^[[:space:]]+mode:/' "$kms_state" > mpv.before
    old_governor=$(cat "$governor_file")
    echo performance > "$governor_file"
    for attempt in {1..4}; do
        printf 'MPV attempt %d\n' "$attempt"
        rc=0
        before_irq=$(irq 169)
        MESA_SHADER_CACHE_DISABLE=true timeout -k 4 50 mpv --no-config -v \
            --msg-level=vo/gpu=debug,ao/alsa=v --msg-time=yes \
            --vo=gpu --gpu-api=opengl --gpu-context=drm --drm-device=/dev/dri/card0 \
            --drm-draw-plane=primary --drm-drmprime-video-plane=overlay \
            --drm-mode=3840x2160@60 --drm-draw-surface-size=1920x1080 \
            --hwdec-codecs=h264,hevc,mpeg2video --hwdec-software-fallback=no \
            --osd-level=0 --swapchain-depth=1 --video-sync=display-resample \
            '--term-status-msg=MPV_STATUS vo_drop=${frame-drop-count} dec_drop=${decoder-frame-drop-count}' \
            --alsa-buffer-time=80000 --alsa-periods=8 --drm-v4l2request-static-g0 --framedrop=no \
            --ao=alsa --audio-device=alsa/hw:0,0 --audio-format=s16 --audio-samplerate=48000 \
            --audio-channels=stereo main10-av.mkv > "mpv-main10-$attempt.log" 2>&1 || rc=$?
        awk '/^plane\[|^[[:space:]]+fb=|^[[:space:]]+mode:/' "$kms_state" > mpv.after
        dmesg > "mpv-dmesg-$attempt.log"
        cmp mpv.before mpv.after
        check_scanout
        test "$(( $(irq 169) - before_irq ))" = 1080
        if python3 mpv_usb_metrics.py "mpv-main10-$attempt.log" "mpv-dmesg-$attempt.log" "$rc" | tee "mpv-metrics-$attempt.json"; then
            passed=1
            break
        fi
    done
    echo "$old_governor" > "$governor_file"
    old_governor=
    test "$passed" = 1
}
