"""Boot the accepted 7.3.6 RAM image and verify MPV's VDEC to V0 path over COM4."""

import argparse
from binascii import crc32
from hashlib import file_digest
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import time

from jpgd_ram_test import LogConsole, checked, ram_boot, stage
from media_regression_ram import BUSYBOX, IRQS, irq_delta, regressions
from serial_boot import PROMPT, SHELL_PROMPT
from vdec_ext_ram import RELEASE, resources, venc_regression
from vdec_ffmpeg_ram import shell
from venc_ram_test import FATAL, PacedSerial, get_stream, verify_boot


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "artifacts"
BASELINE = ARTIFACTS / "arm64-7.3.6-vdec-ext"
VENC_FIXTURE = BASELINE / "venc-regression.nv12"
USB_RELEASE = b"7.3.6-hg680kd-arm64-usb"
USB_PROMPT = b"root@hg680kd:~# "
SNAPSHOT = f"cat /sys/kernel/debug/dri/0/state; cat /proc/asound/card0/pcm0p/sub0/status; {IRQS}"
CHROOT = "/tmp/busybox.static chroot /tmp/mpv-root"
BIND = "/tmp/busybox.static mount -o bind"
PLAYBACK = (
    ("h264-1080p60.mp4", 1080, 60, b"3132564e", 2, False),
    ("mpeg2-1080p30.mkv", 120, 30, b"3132564e", 2, False),
    ("main10-av.mkv", 1080, 60, b"56543048", 4, True),
)


def serial_port():
    port = PacedSerial(baudrate=115200, bytesize=8, parity="N", stopbits=1,
                       timeout=.1, write_timeout=4)
    port.port = "COM4"
    port.dtr = port.rts = False
    return port


def usb_command(console, command, timeout=12):
    console.port.write(f" {command}\r".encode("ascii"))
    return console.expect(USB_PROMPT, timeout)


def start_ram(console, args):
    console.port.write(b"\r")
    prompts = re.compile(b"|".join(map(re.escape, (PROMPT, SHELL_PROMPT, USB_PROMPT))))
    state = console.expect(prompts, 8)
    if state.endswith(USB_PROMPT):
        out = usb_command(console, "unset HISTFILE; uname -r; awk '$2==\"/\" {print}' /proc/mounts; "
                          "for p in ffmpeg mpv; do command -v \"$p\" || echo \"$p missing\"; done; "
                          "dpkg-query -W ffmpeg mpv 2>&1; "
                          "if command -v ffmpeg >/dev/null; then ffmpeg -version | head -4; fi; "
                          "if command -v mpv >/dev/null; then mpv --version | head -4; fi")
        if USB_RELEASE not in out or not re.search(rb"(?m)^/dev/(?:root|sda3) / f2fs ", out):
            raise RuntimeError("serial shell is outside the known USB baseline")
        # Quiesce the pre-existing USB filesystem before a reboot into U-Boot.
        # No task payload is sent until the diskless RAM kernel has been verified.
        usb_command(console, "echo u > /proc/sysrq-trigger")
        for _ in range(10):
            out = usb_command(console, "awk '$1 ~ /^\\/dev\\// {print}' /proc/mounts")
            mounts = re.findall(rb"(?m)^/dev/\S+ (\S+) \S+ (\S+)", out)
            if any(mount == b"/" for mount, _ in mounts) and all(
                    opts.split(b",")[0] == b"ro" for _, opts in mounts):
                break
            time.sleep(1)
        else:
            raise RuntimeError("USB mounts did not become read-only")
        console.port.write(b" echo b > /proc/sysrq-trigger\r")
        console.watch_uboot(55)
    ram_boot(console, args)
    verify_boot(console, RELEASE)


def cpu_load(out):
    samples = re.findall(rb"(?m)^cpu([0-3]) +([\d ]+)", out)
    if b"".join(i for i, _ in samples) != b"01230123":
        raise RuntimeError("missing per-CPU /proc/stat snapshots")
    points = [list(map(int, line.split())) for _, line in samples]
    loads = []
    for before, after in zip(points[:4], points[4:]):
        delta = [end - start for start, end in zip(before[:8], after[:8])]
        total = sum(delta)
        loads.append(round(100 * (total - delta[3] - delta[4]) / total, 2) if total else None)
    return loads


def console_state(out):
    planes = re.findall(rb"(?m)^plane\[(\d+)\]:[^\r\n]*\r?\n\s+crtc=(\S+)\s+fb=(\d+)", out)
    return [(int(plane), crtc.decode(), int(fb)) for plane, crtc, fb in planes]


def playback(console, args, case, report):
    clip, frames, fps, fourcc, planes, audio = case
    filename = f"{Path(clip).stem}.mpv.log"
    status_format = "MPV_STATUS vo_drop=${frame-drop-count} dec_drop=${decoder-frame-drop-count}"
    mpv = (f"{CHROOT} /usr/bin/mpv --no-config -v --msg-level=vo/gpu=debug,ao/alsa=v "
           "--vo=gpu --gpu-api=opengl --gpu-context=drm --drm-device=/dev/dri/card0 "
           "--gpu-hwdec-interop=drmprime-overlay --drm-draw-plane=primary "
           "--drm-drmprime-video-plane=overlay --drm-v4l2request-mv320-tiled "
           "--hwdec=v4l2request --hwdec-codecs=h264,hevc,mpeg2video --hwdec-software-fallback=no "
           "--osd-level=0 --swapchain-depth=1 --video-sync=display-resample "
           f"--term-status-msg={shlex.quote(status_format)} "
           + ("--drm-v4l2request-static-g0 --framedrop=no --ao=alsa --audio-device=alsa/hw:0,0 --audio-format=s16 "
              "--audio-samplerate=48000 --audio-channels=stereo " if audio else "--no-audio ")
           + f"/tmp/{clip}")
    before = resources(console)
    before_kms = checked(console, SNAPSHOT)
    state_before = console_state(before_kms)
    if not state_before or sum(fb != 0 for _, _, fb in state_before) != 1:
        raise RuntimeError(f"unexpected console plane state before playback: {state_before}")
    stats = "grep '^cpu[0-3] ' /proc/stat"
    measurement = (f"{stats}; (sleep 2; {SNAPSHOT}) >/tmp/{filename}.during & sample=$!; "
                   f"MESA_SHADER_CACHE_DISABLE=true {mpv} >/tmp/{filename} 2>&1; "
                   f"ret=$?; {stats}; wait $sample; exit $ret")
    status, measured = shell(console, measurement, max(35, frames // fps + 20))
    after = resources(console)
    after_kms = checked(console, SNAPSHOT)
    state_after = console_state(after_kms)
    running = checked(console, f"cat /tmp/{filename}.during")
    path = args.directory / filename
    get_stream(console, args, path)
    log = path.read_bytes()

    overlay = re.compile(rb"DRM PRIME overlay plane=(\d+) fb=(\d+) pts=([.\d-]+) "
                         rb"fourcc=([a-f0-9]+) modifier=([a-f0-9]+) planes=(\d+)")
    flip = re.compile(rb"DRM flip msc=(\d+) ust_ns=(\d+) video_fb=(\d+)")
    active = []
    pts_by_fb = {}
    unique = []
    for line in log.splitlines():
        update = overlay.search(line)
        if update:
            active.append(update.groups())
            pts_by_fb[int(update[2])] = float(update[3])
        event = flip.search(line)
        if event:
            seq, ns, fb = map(int, event.groups())
            pts = pts_by_fb.get(fb)
            if pts is not None and (not unique or pts != unique[-1][2]):
                unique.append((seq, ns, pts))
    # Cross-check the sequence numbers with the monotonic timestamps and exclude
    # the intentional extra refresh between successive 30 fps frames.
    gaps = [(round((b[1] - a[1]) * 60 / 1e9), round((b[2] - a[2]) * 60))
            for a, b in zip(unique, unique[1:])]
    displayed = len(unique)
    actual_fps = (displayed - 1) * 1e9 / (unique[-1][1] - unique[0][1]) if displayed > 1 else None
    drops = re.findall(rb"MPV_STATUS vo_drop=(\d+) dec_drop=(\d+)", log)
    video_planes = {int(plane) for plane, _, _, _, _, _ in active}
    xruns = len(re.findall(rb"attempt \d+ to recover from state 'XRUN'", log))
    result = {
        "command": mpv, "status": status, "frames_expected": frames,
        "submitted_frames": len({pts for _, _, pts, _, _, _ in active}), "displayed_frames": displayed,
        "fps": round(actual_fps, 3) if actual_fps is not None else None,
        "mpv_drops": list(map(int, drops[-1])) if drops else None,
        "missing_frames": frames - displayed,
        "missed_vblanks": sum(max(actual - expected, 0) for actual, expected in gaps),
        "msc_timestamp_mismatches": sum(b[0] - a[0] != actual
                                        for a, b, (actual, _) in zip(unique, unique[1:], gaps)),
        "cpu_busy_percent": cpu_load(measured), "audio_xruns": xruns if audio else None,
        "vdh_irq_delta": after["vdh_irq"] - before["vdh_irq"],
        "vdp_irq_delta": irq_delta(before_kms, after_kms, 122),
        "aiao_irq_delta": irq_delta(before_kms, after_kms, 116) if audio else None,
        "before": before, "after": after,
        "planes_before": state_before, "planes_during": console_state(running), "planes_after": state_after,
        "video_planes": sorted(video_planes),
    }
    report[clip] = result
    console.record(f"[host] {clip} {displayed}/{frames}, {result['fps']} FPS, "
                   f"{result['missed_vblanks']} missed vblanks, VDH +{result['vdh_irq_delta']}\n".encode())
    if status or b"Using hardware decoding (v4l2request)" not in log or not active or displayed < 2:
        return False
    cma_restored = after["cma_free_kib"] >= before["cma_free_kib"] - 4096
    v0 = re.search(rb"(?m)^plane\[(\d+)\]: V0\r?\n((?:\t[^\n]*\n)+)", running)
    v0_imported = v0 and int(v0[1]) in video_planes and b"imported=yes" in v0[2]
    return (all(fmt == fourcc and mod == b"fe00000000000320" and int(num) == planes
                for _, _, _, fmt, mod, num in active)
            and len(video_planes) == 1 and v0_imported and displayed == frames and abs(actual_fps - fps) < 0.2
            and result["vdh_irq_delta"] == frames and cma_restored and state_before == state_after
            and before["dma_buf"] == after["dma_buf"] == [0, 0] and result["mpv_drops"] == [0, 0]
            and not re.search(rb"Falling back to software|Failed to commit atomic request|"
                              rb"Failed to create framebuffer", log)
            and (not audio or (b"AO: [alsa] 48000Hz stereo 2ch s16" in log and xruns == 0)))


def acceptance(console, args):
    report = {}
    try:
        if not args.reuse_runtime:
            archive = args.directory / "mpv-runtime.tar.gz"
            with archive.open("rb") as stream:
                archive_sha = file_digest(stream, "sha256").hexdigest()
            if archive_sha != (args.directory / "USERSPACE-SHA256SUMS").read_text().split()[0]:
                raise RuntimeError("MPV runtime does not match its build manifest")
            stage(console, args, BUSYBOX, ("busybox.static",))
            checked(console, "chmod 755 /tmp/busybox.static")
            stage(console, args, args.directory, ("mpv-runtime.tar.gz", *(case[0] for case in PLAYBACK)))
            checked(console, "/tmp/busybox.static mkdir -p /tmp/mpv-root && "
                    "/tmp/busybox.static tar -xzf /tmp/mpv-runtime.tar.gz -C /tmp/mpv-root", 100)
        bound = []
        try:
            for name in ("dev", "sys", "proc", "tmp"):
                checked(console, f"{BIND} /{name} /tmp/mpv-root/{name}")
                bound.append(name)
            checked(console, f"{CHROOT} /usr/bin/ffmpeg -version; "
                    f"{CHROOT} /usr/bin/ffmpeg -hwaccels; {CHROOT} /usr/bin/mpv --version; "
                    f"{CHROOT} /usr/bin/mpv --hwdec=help")
            for case in PLAYBACK:
                report[f"{case[0]}_pass"] = playback(console, args, case, report)
        finally:
            for name in reversed(bound):
                checked(console, f"/tmp/busybox.static umount /tmp/mpv-root/{name}")
        regressions(console, args)
        report["lima_jpgd_vdp_player_aiao"] = "pass"
        if not (args.directory / VENC_FIXTURE.name).exists():
            shutil.copyfile(VENC_FIXTURE, args.directory / VENC_FIXTURE.name)
        report["venc"] = venc_regression(console, args)
        report["final_resources"] = resources(console)
        if report["final_resources"]["dma_buf"] != [0, 0] or FATAL.search(checked(console, "dmesg", 55)):
            raise RuntimeError("DMA-BUF leak or kernel fault after regressions")
        if not all(report[f"{case[0]}_pass"] for case in PLAYBACK):
            raise RuntimeError("one or more MPV cases did not meet acceptance")
    finally:
        (args.directory / "mpv-result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8088)
    parser.add_argument("--upload-port", type=int, default=8089)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--boot-only", action="store_true")
    mode.add_argument("--running-ram", action="store_true")
    parser.add_argument("--reuse-runtime", action="store_true")
    args = parser.parse_args()
    if args.reuse_runtime and not args.running_ram:
        parser.error("--reuse-runtime requires --running-ram")
    args.directory = args.directory.resolve()
    image = args.directory / "boot.img"
    image_data = image.read_bytes()
    with image.open("rb") as stream:
        sha = file_digest(stream, "sha256").hexdigest()
    if sha != (BASELINE / "SHA256SUMS").read_text().split()[0]:
        raise RuntimeError("RAM boot image does not match the accepted VDEC-ext image")
    with serial_port() as port, (args.directory / "mpv-com4.log").open("ab") as log:
        console = LogConsole(port, log)
        console.record(f"[host] RAM boot image SHA256 {sha}, local CRC32 {crc32(image_data):08x}\n".encode())
        if args.running_ram:
            port.write(b"\r")
            console.expect(SHELL_PROMPT, 8)
            verify_boot(console, RELEASE)
        else:
            start_ram(console, args)
        if not args.boot_only:
            acceptance(console, args)


if __name__ == "__main__":
    main()
