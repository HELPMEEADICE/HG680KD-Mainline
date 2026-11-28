"""Check frame delivery and ALSA drain in an HG680KD USB MPV run."""

import argparse
import json
from pathlib import Path
import re


def summarize(log, kernel_log, status):
    overlays = re.compile(
        rb"DRM PRIME overlay plane=(\d+) fb=(\d+) pts=([.\d-]+) "
        rb"fourcc=([a-f0-9]+) modifier=([a-f0-9]+) planes=(\d+)"
    )
    flips = re.compile(rb"DRM flip msc=(\d+) ust_ns=(\d+) video_fb=(\d+)")
    submitted = set()
    by_fb = {}
    displayed = []
    layouts = set()
    for line in log.splitlines():
        if update := overlays.search(line):
            plane, fb, pts, fourcc, modifier, planes = update.groups()
            submitted.add(pts)
            by_fb[int(fb)] = float(pts)
            layouts.add((int(plane), fourcc, modifier, int(planes)))
        if event := flips.search(line):
            sequence, timestamp, fb = map(int, event.groups())
            pts = by_fb.get(fb)
            if pts is not None and (not displayed or pts != displayed[-1][2]):
                displayed.append((sequence, timestamp, pts))

    gaps = []
    for first, second in zip(displayed, displayed[1:]):
        measured = round((second[1] - first[1]) * 60 / 1e9)
        gaps.append((second[0] - first[0], measured))
    elapsed_ns = displayed[-1][1] - displayed[0][1] if len(displayed) > 1 else 0
    drops = re.findall(rb"MPV_STATUS vo_drop=(\d+) dec_drop=(\d+)", log)
    timing = re.findall(rb"3840x2160 timing frames=1080 .*?detile=(\d+)", kernel_log)
    tiled = len(layouts) == 1 and next(iter(layouts))[1:] == (b"56543048", b"fe00000000000320", 4)
    return {
        "status": status,
        "v4l2request": b"Using hardware decoding (v4l2request)" in log,
        "tiled_drm_prime": tiled,
        "submitted_frames": len(submitted),
        "displayed_frames": len(displayed),
        "fps": round((len(displayed) - 1) * 1e9 / elapsed_ns, 3) if elapsed_ns > 0 else None,
        "drops": list(map(int, drops[-1])) if drops else None,
        "missed_vblanks": sum(max(measured - 1, 0) for _, measured in gaps),
        "msc_timestamp_mismatches": sum(sequence != measured for sequence, measured in gaps),
        "detile": int(timing[-1]) if timing else None,
        "alsa_buffer_frames": 3840 if b"buffersize: 3840 samples" in log else None,
        "alsa_period_frames": 480 if b"period size: 480 samples" in log else None,
        "alsa_xruns": len(re.findall(rb"attempt \d+ to recover from state 'XRUN'", log)),
        "alsa_drain_completed": log.count(b"ALSA PCM drain completed"),
        "alsa_drain_timeout": log.lower().count(b"drain timeout"),
        "alsa_drain_failed": log.count(b"ALSA PCM drain failed"),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mpv_log", type=Path)
    parser.add_argument("kernel_log", type=Path)
    parser.add_argument("status", type=int)
    args = parser.parse_args()
    metrics = summarize(args.mpv_log.read_bytes(), args.kernel_log.read_bytes(), args.status)
    print(json.dumps(metrics, indent=2))
    expected = {
        "status": 0, "v4l2request": True, "tiled_drm_prime": True,
        "submitted_frames": 1080, "displayed_frames": 1080, "fps": 60.0, "drops": [0, 0],
        "missed_vblanks": 0, "msc_timestamp_mismatches": 0, "detile": 0,
        "alsa_buffer_frames": 3840, "alsa_period_frames": 480, "alsa_xruns": 0,
        "alsa_drain_completed": 1, "alsa_drain_timeout": 0, "alsa_drain_failed": 0,
    }
    return metrics != expected


if __name__ == "__main__":
    raise SystemExit(main())
