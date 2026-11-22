#!/bin/sh
set -eu
export AV_LOG_FORCE_NOCOLOR=1

dir=/tmp
cd "$dir"
run_ffmpeg() {
    echo "+ $dir/ld-musl-aarch64.so.1 --library-path $dir $dir/ffmpeg $*" >&2
    "$dir/ld-musl-aarch64.so.1" --library-path "$dir" "$dir/ffmpeg" "$@"
}

snapshot() {
    grep '^cpu' /proc/stat > "$1.cpu"
    grep -E 'GICv3 +((169)|(181)) ' /proc/interrupts > "$1.irq"
}

last_timing() {
    dmesg | grep 'timing frames=' | {
        last=
        while IFS= read -r line; do last=$line; done
        printf '%s\n' "$last"
    }
}

clocks() {
    for file in /sys/class/thermal/thermal_zone0/temp \
                /sys/devices/system/cpu/cpufreq/policy0/scaling_governor \
                /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq \
                /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq; do
        printf '%s ' "${file##*/}"
        cat "$file"
    done
}

case "$1" in
    probe)
        run_ffmpeg -version
        run_ffmpeg -hwaccels
        ls -l /dev/media0 /dev/video0
        ;;
    check)
        name=$2
        pixels=$3
        output="$name.hw.framemd5"
        snapshot before
        run_ffmpeg -nostdin -hide_banner -loglevel verbose -xerror -max_error_rate 0 \
            -hwaccel v4l2request -hwaccel_output_format drm_prime -i "$name.mp4" \
            -map 0:v:0 -an -sn -dn -vf "hwdownload,format=$pixels" \
            -fps_mode passthrough -threads:v 1 -c:v rawvideo -f framemd5 -y "$output"
        snapshot after
        printf 'HASH FRAMES '
        grep -vc '^#' "$output"
        grep -v '^#' "$output" | cut -d ',' -f 6 | sha256sum
        cat before.irq after.irq
        ;;
    perf)
        name=$2
        run=$3
        printf 'PERF START %s %s\n' "$name" "$run"
        : > perf.progress
        before_timing=$(last_timing)
        clocks
        snapshot before
        read -r wall_before _ < /proc/uptime
        ret=0
        run_ffmpeg -nostdin -hide_banner -nostats -benchmark -xerror -max_error_rate 0 \
            -hwaccel v4l2request -hwaccel_output_format drm_prime -i "$name.mp4" \
            -map 0:v:0 -an -sn -dn -fps_mode passthrough -c:v wrapped_avframe \
            -progress perf.progress -f null - > perf.fflog 2>&1 || ret=$?
        read -r wall_after _ < /proc/uptime
        snapshot after
        clocks
        printf 'WALL %s %s\n' "$wall_before" "$wall_after"
        printf 'CPU BEFORE\n'; cat before.cpu
        printf 'CPU AFTER\n'; cat after.cpu
        printf 'IRQ BEFORE\n'; cat before.irq
        printf 'IRQ AFTER\n'; cat after.irq
        cat perf.fflog
        grep -E '^(frame=|progress=)' perf.progress
        printf 'TIMING BEFORE %s\n' "$before_timing"
        printf 'TIMING AFTER %s\n' "$(last_timing)"
        printf 'PERF END %s %s %s\n' "$name" "$run" "$ret"
        exit "$ret"
        ;;
    *) exit 2 ;;
esac
