"""Validate native HDMI DRM modes on the HG680KD using COM4 and MS2131."""

import argparse
from binascii import crc32
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

import numpy as np

from hdmi_receiver import Receiver, native_snapshot
from jpgd_ram_test import AUDIO, LogConsole, checked, ram_boot, stage
from media_regression_ram import BUSYBOX, mesa_regression, regressions
from mpv_v4l2_drm_ram import BIND, CHROOT, PLAYBACK, playback, serial_port
from vdec_ext_ram import decode, h264_regression, resources
from venc_ffmpeg_ram import run_encode
from venc_ram_test import Case, FATAL, verify_boot

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "artifacts"
RELEASE = b"7.3.6-hg680kd-arm64-hdmi-drm"
CONNECTOR = "/sys/class/drm/card0-HDMI-A-1"
MODETEST = "/tmp/ld-musl-aarch64.so.1 /tmp/modetest -M hi3798mv320-vdp"
SCANOUT = "/sys/bus/platform/devices/f8cc0000.display-controller/scanout"
CPU_GOVERNOR = "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
VDEC = ARTIFACTS / "arm64-7.3.6-vdec-ext"
MPV = ARTIFACTS / "arm64-7.3.6-mpv-drm"
VENC = ARTIFACTS / "arm64-7.3.6-venc-ffmpeg"
MODES = (
    ("1080p60-first", 1920, 1080, 60, 16, 0, 148500),
    ("2160p30", 3840, 2160, 30, 95, 0, 297000),
    ("2160p60-first", 3840, 2160, 60, 97, 3, 297000),
    ("1080p60-return", 1920, 1080, 60, 16, 0, 148500),
    ("2160p60-return", 3840, 2160, 60, 97, 3, 297000),
)


def save_report(directory, report):
    (directory / "acceptance.json").write_text(json.dumps(report, indent=2) + "\n")


def scanout(console):
    raw = checked(console, f"cat {SCANOUT}; grep 'GICv3 *181 ' /proc/interrupts")
    if not all(re.search(pattern, raw) for pattern in (
        rb"\bunderflow_irqs 0\b", rb"fetch_underflow 00000000",
        rb"(?m)^\s*\d+:\s+(?:0\s+){4}GICv3\s+181\b",
    )):
        raise RuntimeError("VDP fetch/scanout underflow or SMMU interrupt")
    return raw


def enumerate_kms(console, directory):
    # GETCONNECTOR probes the EDID again before checking the sysfs mode cache.
    kms = checked(console, f"{MODETEST} -a -c -p")
    raw = checked(console, f"cat {CONNECTOR}/status {CONNECTOR}/modes; "
                  "grep 'hi3798mv320-hpd' /proc/interrupts; "
                  "/tmp/busybox.static test ! -d /sys/kernel/debug/hi3798_hdmi_probe && "
                  "/tmp/busybox.static test ! -d /sys/kernel/debug/hi3798_hdmi_modeset")
    if (not all(field in raw for field in (b"\nconnected", b"\n1920x1080", b"\n3840x2160"))
            or not re.search(rb"GICv3\s+120\s+Level\s+hi3798mv320-hpd\b", raw)):
        raise RuntimeError(f"HDMI connector, modes or INTID 120 missing: {raw!r}")
    edid_hex = checked(console, f"/tmp/busybox.static hexdump -ve '1/1 \"%02x\"' {CONNECTOR}/edid; echo")
    edid = bytes.fromhex(re.search(rb"(?m)^[0-9a-f]{256,}", edid_hex)[0].decode())
    if (len(edid) < 256 or edid[:8] != bytes.fromhex("00ffffffffffff00") or
            len(edid) != 128 * (edid[126] + 1) or edid[128] != 2 or
            any(sum(edid[i:i + 128]) % 256 for i in range(0, len(edid), 128))):
        raise RuntimeError("invalid sysfs EDID base block or CTA extension")
    (directory / "edid.bin").write_bytes(edid)
    (directory / "drm-inventory.txt").write_bytes(raw + kms)
    connector = int(re.search(rb"(?m)^(\d+)\s+\d+\s+connected\s+HDMI-A-1\b", kms)[1])
    state = checked(console, "cat /sys/kernel/debug/dri/0/state")
    crtc = int(re.search(rb"(?m)^crtc\[(\d+)\]: HD0", state)[1])
    g0 = int(re.search(rb"(?m)^plane\[(\d+)\]: G0", state)[1])
    return connector, crtc, g0


def start_modeset(console, mode, ids):
    label, width, height, hz, _, _, _ = mode
    connector, crtc, plane = ids
    scale = 2 if hz == 60 and width == 3840 else 1
    plane_arg = f"{plane}@{crtc}:{width // scale}x{height // scale}*{scale}@XR24"
    cmd = (f"{MODETEST} -a -d -s {connector}@{crtc}:{width}x{height}-{hz} "
           f"-P {shlex.quote(plane_arg)} -F smpte,smpte")
    fifo, log = f"/tmp/{label}.fifo", f"/tmp/{label}.modetest"
    checked(console, f"/tmp/busybox.static rm -f {fifo}; /tmp/busybox.static mkfifo {fifo}; "
            f"(exec 3<>{fifo}; exec {cmd} <&3 >{log} 2>&1) & echo $! >/tmp/{label}.pid")
    return cmd


def stop_modeset(console, label):
    checked(console, f"kill $(cat /tmp/{label}.pid) 2>/dev/null || :")


def record_mode(args, label):
    source = f"video={args.capture_video}:audio={args.capture_audio}"
    with (args.directory / f"{label}-capture.txt").open("wb") as log:
        return subprocess.Popen([
            "ffmpeg", "-hide_banner", "-y", "-f", "dshow", "-rtbufsize", "64M",
            "-video_size", "1920x1080", "-framerate", "30", "-vcodec", "mjpeg",
            "-sample_rate", "48000", "-channels", "2", "-audio_buffer_size", "40",
            "-i", source, "-t", "9", "-c", "copy", str(args.directory / f"{label}.mkv"),
        ], stdin=subprocess.PIPE, stdout=log, stderr=log)


def capture_quality(directory, label):
    movie = str(directory / f"{label}.mkv")
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-i", movie]
    video = subprocess.check_output(cmd + [
        "-ss", "0.30", "-map", "0:v:0", "-vf", "scale=140:90:flags=area",
        "-pix_fmt", "rgb24", "-threads", "1", "-f", "rawvideo", "-",
    ])
    frames = np.frombuffer(video, dtype=np.uint8).reshape(-1, 90, 140, 3)
    # The upper SMPTE field has seven bars, each 1/7 of the raster wide.
    bars = np.array([[1, 1, 1], [1, 1, 0], [0, 1, 1], [0, 1, 0],
                     [1, 0, 1], [1, 0, 0], [0, 0, 1]], dtype=bool)
    columns = np.arange(140).reshape(7, 20)[:, 3:-3]
    samples = frames[:, 3:56, columns, :] >= 96
    good = np.count_nonzero(np.all(samples == bars[None, None, :, None, :], axis=(1, 2, 3, 4)))
    audio = subprocess.check_output(cmd + [
        "-map", "0:a:0", "-f", "s16le", "-acodec", "pcm_s16le", "-",
    ])
    pcm = np.frombuffer(audio, dtype="<i2").reshape(-1, 2).astype(float)
    windows = pcm[:len(pcm) // 96 * 96].reshape(-1, 96, 2)
    audible = np.min(np.sqrt(np.mean(windows**2, axis=1)), axis=1) > 500
    active = np.flatnonzero(audible)
    # Ignore brief samples buffered by the capture card before the test tone.
    active = max(np.split(active, np.flatnonzero(np.diff(active) > 125) + 1), key=len)
    if len(active) < 2750:
        raise RuntimeError(f"No sustained 48 kHz stereo test tone in {label}")
    first, last = active[0] + 125, active[-1] - 125
    signal = pcm[first * 96:last * 96]
    spectrum = np.abs(np.fft.rfft(signal * np.hanning(len(signal))[:, None], axis=0))
    hz = (np.argmax(spectrum, axis=0) * 48000 / len(signal)).tolist()
    return {"frames": len(frames), "intact_bars": int(good), "tone_hz": hz,
            "tone_seconds": (active[-1] - active[0] + 1) / 500,
            "silent_windows_in_tone": int(np.count_nonzero(~audible[first:last]))}


def tx_status(console):
    raw = checked(console, "/tmp/drm-vblank-measure --tx")
    return {key.decode(): int(value, 16) for key, value in re.findall(rb"(\w+)=(0x[0-9a-fA-F]+)", raw)}


def sample_mode(console, args, rx, label):
    recording = record_mode(args, label)
    tone = f"/tmp/{label}-tone.txt"
    try:
        time.sleep(1.5)
        checked(console, "cd /tmp && "
                f"(./ld-musl-aarch64.so.1 --library-path . ./vdp-player --tone 6 >{tone} 2>&1 &)")
        timing = checked(console, "/tmp/drm-vblank-measure", 10)
        vt1 = float(re.search(rb"VT1 waits=121 first=\d+ last=\d+ hz=([\d.]+)", timing)[1])
        regs = tx_status(console)
        sample = native_snapshot(rx)
        try:
            recording.wait(timeout=12)
        except subprocess.TimeoutExpired:
            # DirectShow can stop delivering one of the streams before -t.
            # Ask FFmpeg to close the container before checking captured data.
            recording.communicate(input=b"q", timeout=8)
        if recording.returncode:
            raise RuntimeError(f"DirectShow capture failed in {label}")
    finally:
        if recording.poll() is None:
            recording.kill()
            recording.wait()

    pcm = checked(console, f"cat {tone}")
    quality = capture_quality(args.directory, label)
    return vt1, regs, sample, pcm, quality


def check_mode(console, args, rx, mode, ids, current, report):
    label, width, height, hz, vic, colorspace, cts = mode
    cmd = start_modeset(console, mode, ids)
    current.append(label)
    time.sleep(1)
    state = checked(console, "cat /sys/kernel/debug/dri/0/state")
    (args.directory / f"{label}-drm-state.txt").write_bytes(state)
    rx_mode = native_snapshot(rx)
    if (rx_mode["pixel_active"] != [width, height] or rx_mode["vic"] != vic or
            rx_mode["avi_colorspace"] != colorspace):
        raise RuntimeError(f"{label}: receiver did not switch: {rx_mode}")
    if len(current) > 1:
        stop_modeset(console, current.pop(0))

    for attempt in (f"{label}-capture1", label):
        try:
            vt1, regs, sample, pcm, quality = sample_mode(console, args, rx, attempt)
        except (subprocess.SubprocessError, RuntimeError):
            if attempt == label:
                raise
            time.sleep(1)
            continue
        capture_pass = (quality["intact_bars"] == quality["frames"] and quality["frames"] > 200 and
                        quality["silent_windows_in_tone"] == 0 and
                        all(abs(value - 1000) < 0.5 for value in quality["tone_hz"]))
        if capture_pass:
            if attempt != label:
                (args.directory / f"{attempt}.mkv").replace(args.directory / f"{label}.mkv")
                (args.directory / f"{attempt}-capture.txt").replace(args.directory / f"{label}-capture.txt")
            break

    report[label] = {"command": cmd, "tx": regs, "receiver": sample, "vt1_hz": vt1,
                     "alsa": pcm.decode(errors="replace"), **quality}
    save_report(args.directory, report)
    scanout(console)
    if not (
        sample["geometry_consistent"] and sample["avi_checksum_ok"] and sample["drm_checksum_ok"] and
        sample["drm_eotf"] == 0 and sample["drm_max_cll"] == 0 and sample["gcp_reported_bpc"] == 8 and
        sample["vic"] == vic and sample["avi_colorspace"] == colorspace and
        sample["rx_acr_n"] == 6144 and abs(sample["rx_acr_cts"] - cts) <= 1 and
        round(sample["acr_8bpc_frame_hz"], 3) == hz and abs(vt1 - hz) < 0.02 and
        regs["hpll"] & 8 and regs["phy"] & (1 << 20) and regs["n"] & 0xfffff == 6144 and
        regs["sw_cts"] & 0xfffff == cts and abs((regs["hw_cts"] & 0xfffff) - cts) <= 8 and
        not any(regs[x] for x in ("req", "ack", "arb")) and not (regs["encoder"] & 0x27) and
        b"ALSA played=288000 frames rate=48000 xruns=0" in pcm and capture_pass
    ):
        raise RuntimeError(f"{label} acceptance failed")
    print(f"{label}: VIC {vic}, VT1 {vt1:.6f} Hz, CTS {cts}, "
          f"{quality['frames']} intact frames, zero XRUN", flush=True)


def pcm_across_modeset(console, rx, ids, current):
    pcm = "/proc/asound/card0/pcm0p/sub0/status"
    tone = "/tmp/continuous-tone.txt"
    checked(console, "cd /tmp && "
            f"(./ld-musl-aarch64.so.1 --library-path . ./vdp-player --tone 12 >{tone} 2>&1 &)")
    time.sleep(0.5)
    status = [checked(console, f"cat {pcm}")]
    for label, mode in (("pcm-1080p60", MODES[0]), ("pcm-2160p60", MODES[-1])):
        start_modeset(console, (label, *mode[1:]), ids)
        current.append(label)
        time.sleep(1)
        if native_snapshot(rx)["vic"] != mode[4]:
            raise RuntimeError("Mode did not change during continuous PCM")
        stop_modeset(console, current.pop(0))
        status.append(checked(console, f"cat {pcm}"))

    owners = [re.search(rb"owner_pid\s*:\s*(\d+)", snapshot) for snapshot in status]
    if (any(b"state: RUNNING" not in snapshot for snapshot in status)
            or any(not owner for owner in owners)
            or len({owner[1] for owner in owners}) != 1):
        raise RuntimeError("PCM stream did not stay open across the modesets")
    out = checked(console, f"sleep 10; cat {tone}", 15)
    if b"ALSA played=576000 frames rate=48000 xruns=0" not in out:
        raise RuntimeError("Continuous AIAO playback failed")
    return {"frames": 576000, "rate": 48000, "xruns": 0, "modes": [97, 16, 97]}


def native_mpv(console, args, report):
    case = PLAYBACK[-1]
    clip = case[0]
    stage(console, args, MPV, ("mpv-runtime.tar.gz", clip))
    stable = checked(console, "for i in 1 2 3 4 5 6 7 8; do "
                     f"{MODETEST} -a -c | grep '3840x2160 60.00'; done", 30)
    (args.directory / "edid-reprobe.txt").write_bytes(stable)
    if len(re.findall(rb"(?m)^\s+#\d+ 3840x2160 60\.00 ", stable)) != 8:
        raise RuntimeError("2160p60 disappeared during repeated GETCONNECTOR probes")
    checked(console, "/tmp/busybox.static mkdir -p /tmp/mpv-root && "
            "/tmp/busybox.static tar -xzf /tmp/mpv-runtime.tar.gz -C /tmp/mpv-root", 100)
    bound = []
    governor = None
    try:
        for name in ("dev", "sys", "proc", "tmp"):
            checked(console, f"{BIND} /{name} /tmp/mpv-root/{name}")
            bound.append(name)
        checked(console, f"{CHROOT} /usr/bin/mpv --version")
        raw_governor = checked(console, f"cat {CPU_GOVERNOR}")
        governor = re.search(rb"(?m)^([a-z_]+)\r?$", raw_governor)[1].decode()
        checked(console, f"echo performance > {CPU_GOVERNOR}")
        report["mpv_governor"] = "performance"
        attempts = report["mpv_attempts"] = []
        pcm = ("--alsa-buffer-time=80000", "--alsa-periods=8")
        options = (
            (),
            ("--audio-stream-silence=yes",),
            ("--audio-stream-silence=yes", "--cache=yes", "--cache-pause-initial=yes"),
        )
        for attempt, extra in enumerate(options, 1):
            passed = playback(console, args, case, report, native_uhd=True,
                              extra_options=pcm + extra)
            result = report[clip]
            log = checked(console, "dmesg | grep 'timing frames='", 25)
            (args.directory / "mpv-vdec-timing.txt").write_bytes(log)
            timing = re.findall(rb"(?m)^.*3840x2160 timing frames=1080 .*?detile=(\d+)", log)
            result["detile_ns_per_frame"] = int(timing[-1]) if timing else None
            attempts.append(result)
            shutil.copyfile(args.directory / "main10-av.mpv.log",
                            args.directory / f"main10-av-attempt{attempt}.mpv.log")
            scanout(console)
            save_report(args.directory, report)
            if (passed and not result["missed_vblanks"] and not result["msc_timestamp_mismatches"]
                    and result["fps"] == 60.0 and result["detile_ns_per_frame"] == 0):
                break
        else:
            raise RuntimeError(f"Native DRM MPV failed {clip}: {result}")
    finally:
        if governor:
            checked(console, f"echo {governor} > {CPU_GOVERNOR}")
        for name in reversed(bound):
            checked(console, f"/tmp/busybox.static umount /tmp/mpv-root/{name}")
        checked(console, "/tmp/busybox.static rm -rf /tmp/mpv-root /tmp/mpv-runtime.tar.gz", 30)


def media_regression(console, args, report):
    regressions(console, args, player_dir=args.directory)
    report["lima_jpgd_vdp_aiao"] = "pass"
    report["mesa_irq_delta"] = mesa_regression(console, args)
    checked(console, "/tmp/busybox.static rm -rf /tmp/mesa /tmp/mesa-runtime.tar.gz", 30)
    report["h264"] = h264_regression(console, args, "h264-1080p60.mp4", 1080, 60)
    if not report["h264"]["pass"]:
        raise RuntimeError(f"H.264 regression failed: {report['h264']}")
    scanout(console)
    names = ("mpeg1-1080p", "mpeg2-1080p")
    stage(console, args, VDEC, ("vdec-mpeg-test", *(f"{name}.requests" for name in names)))
    checked(console, "chmod 755 /tmp/vdec-mpeg-test")
    for name in names:
        for suffix in ("requests", "ref.nv21"):
            shutil.copyfile(VDEC / f"{name}.{suffix}", args.directory / f"{name}.{suffix}")
        report[name] = decode(console, args, name)
        save_report(args.directory, report)

    raw = VENC / "1080p-nv12-qp32.raw"
    stage(console, args, VENC, ("ffmpeg", "venc-ioctl-smoke", raw.name + ".gz"))
    checked(console, "cd /tmp && chmod 755 ffmpeg venc-ioctl-smoke && "
            f"./busybox.static gzip -dc {raw.name}.gz >{raw.name} && "
            f"./busybox.static rm {raw.name}.gz", 75)
    args.profile, args.trace, args.probe = True, False, False
    try:
        report["ffmpeg_venc"], _ = run_encode(console, args, Case(1920, 1080, "NV12", 28), raw, ".mp4")
    finally:
        checked(console, f"/tmp/busybox.static rm /tmp/{raw.name}")
    scanout(console)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8088)
    parser.add_argument("--upload-port", type=int, default=8089)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--capture-video", default="USB2 Video")
    parser.add_argument("--capture-audio", default="数字音频接口 (USB2 Digital Audio)")
    parser.add_argument("--running-ram", action="store_true")
    parser.add_argument("--switch-only", action="store_true")
    args = parser.parse_args()
    args.directory = args.directory.resolve()
    image = (args.directory / "boot.img").read_bytes()
    digest = sha256(image).hexdigest()
    if digest != (args.directory / "BOOT-SHA256SUMS").read_text().split()[0]:
        raise RuntimeError("HDMI DRM boot image SHA256 mismatch")
    report = {"sha256": digest, "local_crc32": f"{crc32(image):08x}"}
    current = []
    failures = {}
    with serial_port() as port, (args.directory / "board-com4.txt").open("ab") as log, Receiver() as rx:
        console = LogConsole(port, log)
        if not args.running_ram:
            ram_boot(console, args)
        verify_boot(console, RELEASE)
        stage(console, args, args.directory, ("modetest", "drm-vblank-measure", "vdp-player"))
        stage(console, args, BUSYBOX, ("busybox.static",))
        stage(console, args, ARTIFACTS / "arm64-7.3.6-usb-full", tuple(p for p in AUDIO if p != "vdp-player"))
        checked(console, "cd /tmp && chmod 755 modetest drm-vblank-measure busybox.static "
                "ld-musl-aarch64.so.1 vdp-player")
        ids = enumerate_kms(console, args.directory)
        report["initial_resources"] = resources(console)
        scanout(console)
        try:
            for mode in MODES:
                check_mode(console, args, rx, mode, ids, current, report)
            if not args.switch_only:
                try:
                    report["continuous_pcm"] = pcm_across_modeset(console, rx, ids, current)
                except RuntimeError as error:
                    failures["continuous_pcm"] = str(error)
                try:
                    native_mpv(console, args, report)
                except RuntimeError as error:
                    failures["mpv"] = str(error)
        finally:
            for label in current:
                stop_modeset(console, label)
            save_report(args.directory, report)
        if not args.switch_only:
            report["before_media_resources"] = resources(console)
            try:
                media_regression(console, args, report)
            finally:
                save_report(args.directory, report)
        report["final_resources"] = resources(console)
        state = checked(console, "cat /sys/kernel/debug/dri/0/state")
        (args.directory / "console-restored.txt").write_bytes(state)
        if b"allocated by = [fbcon]" not in state or b'mode: "1920x1080": 60 148500' not in state:
            raise RuntimeError("The 1080p60 framebuffer console did not return")
        final = checked(console, f"dmesg; cat {SCANOUT} /proc/mounts /proc/partitions", 55)
        (args.directory / "final.txt").write_bytes(final)
        scanout(console)
        if failures:
            report["failures"] = failures
        save_report(args.directory, report)
        # Lima retains the last task VM across userspace exits. After the first
        # MPV run, measure recovery against the same warmed GPU state.
        baseline = report.get("before_media_resources", report["initial_resources"])
        if (report["final_resources"]["dma_buf"] != [0, 0] or FATAL.search(final)
                or baseline["cma_used_pages"] != report["final_resources"]["cma_used_pages"]):
            raise RuntimeError("resource recovery or kernel log check failed")
        if failures:
            raise RuntimeError(f"Acceptance failures: {failures}")


if __name__ == "__main__":
    main()
