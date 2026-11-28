"""Boot the HG680KD RAM modeset probe and collect COM4 and HDMI evidence."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import wave

import numpy as np
from PIL import Image
from receiver import receiver_snapshot

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from jpgd_ram_test import AUDIO, LogConsole, checked, ram_boot, stage
from vdec_ffmpeg_ram import shell
from vdp_ram_test import count_irq
from venc_ram_test import PacedSerial

PROBE = "/sys/kernel/debug/hi3798_hdmi_modeset"
PLL_STATS = "grep -E 'GICv3 +(116|122|181) ' /proc/interrupts"
SNAPSHOT = f"cat {PROBE}; {PLL_STATS}; cat /proc/asound/card0/pcm0p/sub0/status"
TONE = ("cd /tmp && ./ld-musl-aarch64.so.1 --library-path . ./vdp-player "
        "--tone 8 >/tmp/modeset-tone.txt 2>&1")
MODES = (("1080p60-first", "1080p60", 60, 148500, 16),
         ("2160p30", "2160p30", 30, 297000, 95),
         ("1080p60-return", "1080p60", 60, 148500, 16))
COLORS = np.array(((255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
                   (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)))


def capture(args, label, audio=False):
    source = f"video={args.capture_video}"
    if audio:
        source += f":audio={args.capture_audio}"
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "info", "-nostdin", "-y",
           "-f", "dshow", "-rtbufsize", "64M", "-video_size", "1920x1080",
           "-framerate", "30", "-vcodec", "mjpeg"]
    if audio:
        cmd += ["-sample_rate", "48000", "-channels", "2", "-audio_buffer_size", "40"]
    with (args.directory / f"{label}-capture.txt").open("wb") as log:
        return subprocess.Popen(cmd + ["-i", source, "-t", "12" if audio else "2",
                                      "-c", "copy", str(args.directory / f"{label}.mkv")],
                                stdout=log, stderr=log)


def ffmpeg(*args):
    return subprocess.check_output(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
                                    "-y", *map(str, args)])


def analyze(directory, label):
    movie = directory / f"{label}.mkv"
    still = directory / f"{label}.png"
    ffmpeg("-ss", "5", "-i", movie, "-frames:v", "1", "-threads", "1", still)
    # Sample 24 interior points from every frame after the first 250 ms.
    video = ffmpeg("-ss", "0.25", "-i", movie, "-map", "0:v:0",
                   "-vf", "scale=160:90:flags=area", "-pix_fmt", "rgb24",
                   "-threads", "1", "-f", "rawvideo", "-")
    frames = np.frombuffer(video, dtype=np.uint8).reshape(-1, 90, 160, 3)
    samples = frames[:, [12, 45, 77], :][:, :, np.arange(10, 160, 20)]
    matching = np.all((samples >= 128) == (COLORS >= 128), axis=(1, 2, 3))
    interior = np.arange(160).reshape(8, 20)[:, 3:-3]
    good_interior = ((frames[:, :, interior, :] >= 128) ==
                     (COLORS[None, None, :, None, :] >= 128))
    intact = np.all(good_interior, axis=(1, 2, 3, 4))
    still_array = np.asarray(Image.open(still).convert("RGB"))
    y = still_array.shape[0] // 2
    points = still_array[y, (np.arange(8) * 2 + 1) * still_array.shape[1] // 16].tolist()

    wav = directory / f"{label}.wav"
    ffmpeg("-i", movie, "-map", "0:a:0", "-c:a", "pcm_s16le", wav)
    with wave.open(str(wav), "rb") as stream:
        if (stream.getframerate(), stream.getnchannels(), stream.getsampwidth()) != (48000, 2, 2):
            raise RuntimeError("Unexpected HDMI audio capture format")
        samples = np.frombuffer(stream.readframes(stream.getnframes()),
                                dtype="<i2").reshape(-1, 2).astype(float)
    windows = samples[:len(samples) // 96 * 96].reshape(-1, 96, 2)
    rms = np.sqrt(np.mean(windows ** 2, axis=1))
    active = np.flatnonzero(np.min(rms, axis=1) > 500)
    if len(active) and active[-1] - active[0] > 250:
        steady = rms[active[0] + 125:active[-1] - 125]
        tone = samples[(active[0] + 125) * 96:(active[-1] - 125) * 96]
        fft = np.abs(np.fft.rfft(tone * np.hanning(len(tone))[:, None], axis=0))
        frequencies = (np.argmax(fft, axis=0) * 48000 / len(tone)).tolist()
        audio_seconds = (active[-1] - active[0] + 1) * .002
        quiet_windows = int(np.sum(np.min(steady, axis=1) < 500))
        peak = int(np.max(np.abs(tone)))
        max_step = int(np.max(np.abs(np.diff(tone, axis=0))))
    else:
        audio_seconds, quiet_windows, peak, max_step, frequencies = 0, None, None, None, []
    return {"frames": len(frames), "matching_bars": int(matching.sum()),
            "intact_bar_frames": int(intact.sum()), "RGB_samples": points,
            "audio_seconds": round(audio_seconds, 3), "fft_hz": frequencies,
            "quiet_2ms_windows": quiet_windows, "steady_peak": peak, "steady_max_step": max_step}


def measurements(before, during, after, nominal, clock, vic, colorspace=0):
    def field(raw, name):
        return int(re.search(rb"(?m)^" + re.escape(name.encode()) + rb"=([a-f0-9]+)", raw)[1], 16)

    stats = re.search(rb"vt1=(\d+) first_ns=(\d+) last_ns=(\d+) underflow=(\d+) fetch=([a-f0-9]+)", after)
    count, _, last, underflow = map(int, stats.groups()[:4])
    previous = re.search(rb"vt1=(\d+) first_ns=\d+ last_ns=(\d+)", before)
    previous_count, previous_ns = map(int, previous.groups())
    rate = (count - previous_count) * 1e9 / (last - previous_ns)
    header = field(after, "packet[018]").to_bytes(4, "little")[:3]
    body = (field(after, "packet[01c]").to_bytes(4, "little") +
            field(after, "packet[020]").to_bytes(4, "little")[:3] + bytes(7))
    cts_values = [int(x, 16) for x in re.findall(rb"(?m)^audio\[050\]=([a-f0-9]+)", during)]
    clocks = re.search(rb"pixel_count=(\d+) tmds_count=(\d+)", after)
    pixel_khz, tmds_khz = map(int, clocks.groups())
    h, v = field(after, "video[0c0]"), field(after, "video[0c4]")
    h_scale = 2 if colorspace == 3 else 1
    result = {"pixel_khz": pixel_khz, "tmds_khz": tmds_khz,
              "active": [h & 0x1fff, v & 0x1fff],
              "totals": [h >> 13 & 0x1fff, v >> 13 & 0x1fff],
              "vt1_count": count, "vt1_hz": rate, "underflow": underflow,
              "fetch_underflow": int(stats[5], 16),
              "smmu_181": count_irq(before + during + after, 181),
              "aiao_irqs": count_irq(before + after, 116),
              "n": field(after, "audio[048]"), "sw_cts": field(after, "audio[04c]"),
              "hw_cts_playing": cts_values, "hw_cts_idle": field(after, "audio[050]"),
              "avi": (header + body).hex(" "), "tx_stable": field(after, "packet[004]") & 1,
              "crg_150": field(after, "crg[150]"),
              "xruns": 0 if b"ALSA played=384000 frames rate=48000 xruns=0" in after else None}
    ok = (abs(rate - nominal) < .05 and underflow == result["fetch_underflow"] == 0
          and result["smmu_181"] == [0, 0, 0] and len(result["aiao_irqs"]) == 2
          and result["aiao_irqs"][1] > result["aiao_irqs"][0]
          and result["n"] == 6144 and result["sw_cts"] == clock
          and len(cts_values) == 5 and all(abs(value - clock) <= 8 for value in cts_values)
          and header == bytes((0x82, 2, 13)) and sum(header + body) % 256 == 0
          and body[1] == (colorspace << 5 | 2) and body[2] == 0xa0
          and body[3] == (0 if colorspace == 3 else 4) and body[4] == vic
          and result["tx_stable"] == 1 and result["xruns"] == 0
          and field(after, "phy[064]") & 0x7f80f == 0x800
          and not field(after, "packet[1b4]") & 3 and not field(after, "packet[260]") & 3
          and result["active"] == ([1920, 1080] if vic == 16 else [3840, 2160])
          and result["totals"] == ([2200, 1125] if vic == 16 else [4400, 2250])
          and abs(pixel_khz - clock * h_scale) <= 1 and abs(tmds_khz - clock) <= 1
          and result["crg_150"] & 0x7f == 0x7f
          and bool(re.search(rb"(?m)^closed\r?$", after)))
    result["board_pass"] = ok
    return result


def run_mode(console, args, label, mode, nominal, clock, vic, colorspace=0):
    print(f"Switching to {mode} ({label})", flush=True)
    rc, output = shell(console, f"echo {mode} > {PROBE}; rc=$?; cat {PROBE}; (exit $rc)", 15)
    (args.directory / f"{label}-switch.txt").write_bytes(output)
    if rc or not re.search(rb"(?m)^mode=" + mode.encode() + rb" step=active error=0", output):
        raise RuntimeError(f"Mode switch failed, see {label}-switch.txt")
    time.sleep(1)
    before = checked(console, SNAPSHOT)
    recording = capture(args, label, True)
    try:
        time.sleep(2)
        rx_before = receiver_snapshot()
        checked(console, f"({TONE} &)")
        during = checked(console, f"{PLL_STATS}; for n in 1 2 3 4 5; do "
                         rf"grep '^audio\[050\]' {PROBE}; sleep 1; done; "
                         "cat /proc/asound/card0/pcm0p/sub0/status")
        rx_during = receiver_snapshot()
        after = checked(console, "for n in 1 2 3 4 5 6 7 8 9 10 11 12; do "
                        "grep -q 'ALSA played=' /tmp/modeset-tone.txt && break; sleep 1; done; "
                        f"cat /tmp/modeset-tone.txt; {SNAPSHOT}", 18)
        if recording.wait(timeout=18):
            raise RuntimeError(f"DirectShow capture failed, see {label}-capture.txt")
    finally:
        if recording.poll() is None:
            recording.kill()
            recording.wait()

    (args.directory / f"{label}-snapshot.txt").write_bytes(before + during + after)
    result = measurements(before, during, after, nominal, clock, vic, colorspace)
    result["rx_before"] = rx_before
    result["rx_during"] = rx_during
    result.update(analyze(args.directory, label))
    result["capture_pass"] = (result["frames"] > 300 and
                              result["frames"] == result["matching_bars"] and
                              7.85 < result["audio_seconds"] < 8.10 and
                              result["quiet_2ms_windows"] == 0 and
                              result["steady_max_step"] < 2500 and
                              all(abs(hz - 1000) < .5 for hz in result["fft_hz"]))
    return result


def check_transition_audio(console, args):
    """Keep the USB PCM capture open across two additional modesets."""
    wav = args.directory / "transition-audio.wav"
    with (args.directory / "transition-audio-capture.txt").open("wb") as log:
        recording = subprocess.Popen([
            "ffmpeg", "-hide_banner", "-nostdin", "-y", "-f", "dshow", "-sample_rate", "48000",
            "-channels", "2", "-audio_buffer_size", "40", "-i", f"audio={args.capture_audio}",
            "-t", "21", "-c:a", "pcm_s16le", str(wav),
        ], stdout=log, stderr=log)
    try:
        time.sleep(2)
        for mode in ("1080p60", "2160p30", "1080p60"):
            checked(console, f"echo {mode} > {PROBE}")
            time.sleep(1)
            output = checked(console, TONE.replace("--tone 8", "--tone 3") +
                             " && cat /tmp/modeset-tone.txt", 12)
            if b"ALSA played=144000 frames rate=48000 xruns=0" not in output:
                raise RuntimeError(f"Transition audio test failed at {mode}")
            time.sleep(.5)
        if recording.wait(timeout=18):
            raise RuntimeError("Continuous HDMI audio capture failed")
    finally:
        if recording.poll() is None:
            recording.kill()
            recording.wait()

    with wave.open(str(wav), "rb") as stream:
        if (stream.getframerate(), stream.getnchannels(), stream.getsampwidth()) != (48000, 2, 2):
            raise RuntimeError("Unexpected transition audio format")
        samples = np.frombuffer(stream.readframes(stream.getnframes()),
                                dtype="<i2").reshape(-1, 2).astype(float)
    windows = samples[:len(samples) // 96 * 96].reshape(-1, 96, 2)
    rms = np.sqrt(np.mean(windows ** 2, axis=1))
    active = np.flatnonzero(np.min(rms, axis=1) > 500)
    runs = np.split(active, np.flatnonzero(np.diff(active) > 125) + 1)
    tones = [(int(run[0]), int(run[-1])) for run in runs if len(run) > 1000]
    gaps = [windows[end + 50:start - 50] for (_, end), (start, _) in zip(tones, tones[1:])]
    peaks = [int(np.max(np.abs(gap))) for gap in gaps]
    result = {"tones_seconds": [[lo * .002, hi * .002] for lo, hi in tones],
              "switch_gap_peaks": peaks, "pcm_full_scale": 32768}
    result["pass"] = len(tones) == 3 and len(peaks) == 2 and max(peaks) < 500
    (args.directory / "transition-audio.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    return result["pass"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--capture-video", default="USB2 Video")
    parser.add_argument("--capture-audio", default="数字音频接口 (USB2 Digital Audio)")
    parser.add_argument("--kernel-release", default="7.3.6-hg680kd-arm64-hdmi-modeset")
    parser.add_argument("--running-ram", action="store_true")
    parser.add_argument("--transition-audio", action="store_true")
    parser.add_argument("--start-at", choices=tuple(row[0] for row in MODES), default=MODES[0][0])
    args = parser.parse_args()

    results = {}
    port = PacedSerial(port=None, baudrate=115200, bytesize=8, parity="N", stopbits=1,
                       timeout=.08, write_timeout=3)
    port.port = "COM4"
    port.dtr = port.rts = False
    with port, (args.directory / "board-com4.txt").open("ab" if args.running_ram else "wb") as log:
        console = LogConsole(port, log)
        if not args.running_ram:
            recording = capture(args, "uboot")
            try:
                if recording.wait(timeout=15):
                    raise RuntimeError("U-Boot HDMI capture failed")
            finally:
                if recording.poll() is None:
                    recording.kill()
                    recording.wait()
            (args.directory / "uboot-receiver.json").write_text(
                json.dumps(receiver_snapshot(), indent=2) + "\n")
            ram_boot(console, args)
        out = checked(console, "uname -r; cat /proc/mounts /proc/partitions")
        if (not re.search(rb"(?m)^" + re.escape(args.kernel_release.encode()) + rb"\r?$", out) or
                b"rootfs / rootfs " not in out or
                re.search(rb"(?m)^/dev/\S+|^\s+\d+\s+\d+\s+\d+\s+\w+\r?$", out)):
            raise RuntimeError("Expected the storage-free modeset RAM image")
        checked(console, "dmesg -n 4; ls /sys/kernel/debug/clk >/dev/null 2>&1 || "
                         "mount -t debugfs debugfs /sys/kernel/debug")
        (args.directory / "firmware-snapshot.txt").write_bytes(checked(console, SNAPSHOT))
        stage(console, args, SCRIPTS.parent / "artifacts/arm64-7.3.6-usb-full", AUDIO)
        checked(console, "chmod 755 /tmp/vdp-player /tmp/ld-musl-aarch64.so.1")
        for row in MODES:
            if not results and row[0] != args.start_at:
                continue
            results[row[0]] = run_mode(console, args, *row)
            print(json.dumps(results[row[0]]), flush=True)
            (args.directory / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        transition_pass = not args.transition_audio or check_transition_audio(console, args)
        out = checked(console, f"dmesg; {SNAPSHOT}; cat /proc/mounts /proc/partitions", 25)
        (args.directory / "final.txt").write_bytes(out)
        if re.search(rb"SError|external abort|Kernel panic|BUG:|WARNING:|disabling IRQ", out):
            raise RuntimeError("Kernel error in final log")
        if count_irq(out, 181) != [0] or b"underflow=0 fetch=00000000" not in out:
            raise RuntimeError("Final hardware error counters are nonzero or missing")
    return int(not (transition_pass and all(r["board_pass"] and r["capture_pass"]
                                            for r in results.values())))


if __name__ == "__main__":
    raise SystemExit(main())
