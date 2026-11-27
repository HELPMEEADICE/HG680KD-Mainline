#!/usr/bin/env bash
set -euo pipefail

source_video=${1:?supply the Blender 2160p60 Big Buck Bunny source}
out=${2:?supply the output directory}
mkdir -p "$out"

if [[ ${3:-} == color ]]; then
    work=$(mktemp -d)
    trap 'rm -f -- "$work"/{709,601,2020}.mkv "$work/inputs"; rmdir -- "$work"' EXIT
    section() {
        local name=$1 offset=$2 matrix=$3 colorspace=$4 range=$5

        ffmpeg -nostdin -hide_banner -loglevel warning -threads 4 -ss "$offset" -i "$source_video" \
            -t 2 -map 0:v:0 -an -filter_threads 2 \
            -vf "scale=1920:1080:in_color_matrix=bt709:out_color_matrix=$matrix:in_range=tv:out_range=$range" \
            -colorspace "$colorspace" -color_range "$range" -color_primaries bt709 -color_trc bt709 \
            -c:v libx265 -pix_fmt yuv420p10le -preset ultrafast \
            -x265-params 'pools=4:frame-threads=2:keyint=120:scenecut=0:open-gop=0:bframes=0:repeat-headers=1' \
            "$work/$name.mkv"
    }
    section 709 45 bt709 bt709 tv
    section 601 47 bt601 smpte170m pc
    section 2020 49 bt2020 bt2020nc tv
    printf 'file 709.mkv\nfile 601.mkv\nfile 2020.mkv\nfile 709.mkv\n' > "$work/inputs"
    ffmpeg -nostdin -hide_banner -loglevel warning -y -f concat -i "$work/inputs" \
        -map 0:v:0 -c copy "$out/color-switch.mkv"
    exit
fi

encode() {
    ffmpeg -nostdin -hide_banner -loglevel warning -y -threads 4 -ss 45 -i "$source_video" \
        -t 18 -map 0:v:0 -an -filter_threads 2 -color_range tv \
        -colorspace bt709 -color_primaries bt709 -color_trc bt709 "$@"
}

hevc='pools=8:frame-threads=3:keyint=120:min-keyint=120:scenecut=0:open-gop=0:bframes=3:ref=3:vbv-maxrate=24000:vbv-bufsize=24000:level-idc=5.1'
encode -c:v libx265 -pix_fmt yuv420p10le -preset ultrafast -crf 26 -x265-params "$hevc" \
    "$out/main10-2160p60.mkv" &
main10=$!
encode -c:v libx265 -pix_fmt yuv420p -preset ultrafast -crf 26 -x265-params "$hevc" \
    -tag:v hvc1 -movflags +faststart "$out/main-2160p60.mp4" &
main=$!
encode -vf scale=1920:1080 -c:v libx264 -pix_fmt yuv420p -preset veryfast -crf 20 -threads 6 \
    -x264-params 'keyint=120:min-keyint=120:scenecut=0:open-gop=0:bframes=3:ref=3:direct=auto' \
    -movflags +faststart "$out/h264-1080p60.mp4" &
h264=$!
result=0
for job in "$main10" "$main" "$h264"; do
    wait "$job" || result=1
done
(( result == 0 ))

cd "$out"
for file in main10-2160p60.mkv main-2160p60.mp4 h264-1080p60.mp4; do
    printf '\n%s\n' "$file"
    ffprobe -v error -count_packets -select_streams v:0 -show_entries \
        stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,time_base,nb_read_packets,color_space,color_range:format=duration \
        -of compact "$file"
done
sha256sum main10-2160p60.mkv main-2160p60.mp4 h264-1080p60.mp4
