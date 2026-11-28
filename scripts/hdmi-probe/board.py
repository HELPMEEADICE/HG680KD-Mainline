"""Run the HDMI probe with color bars and a continuous AIAO tone on HG680KD."""

import argparse
import hashlib
from ipaddress import IPv4Address
import os
from pathlib import Path
import re
import subprocess
import sys
import time

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from jpgd_ram_test import LogConsole, checked, ram_boot, stage
from vdec_ffmpeg_ram import shell
from vdp_ram_test import SCANOUT, count_irq
from venc_ram_test import PacedSerial, verify_boot


ARTIFACTS = SCRIPTS.parent / "artifacts"
RELEASE = b"7.3.6-hg680kd-arm64-hdmi-probe"
PROBE = "/sys/kernel/debug/hi3798_hdmi_probe"
CURSOR = "/sys/class/graphics/fbcon/cursor_blink"
SNAPSHOT = ("grep -E 'GICv3 +(116|120|122|179|181) ' /proc/interrupts; "
            f"grep -E '^(vblank_irqs|fetch_underflow|c228 )' {SCANOUT}; cat /proc/uptime")
AUDIO = ("vdp-player", "ld-musl-aarch64.so.1", "libdrm.so.2", "libudev.so.1")


def record_capture(args):
    if not args.capture_video:
        return None
    source = f"video={args.capture_video}"
    if args.capture_audio:
        source += f":audio={args.capture_audio}"
    with (args.directory / "hdmi-capture.txt").open("wb") as errors:
        capture = subprocess.Popen([
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning", "-y",
            "-f", "dshow", "-rtbufsize", "64M", "-video_size", "1920x1080",
            "-framerate", "30", "-vcodec", "mjpeg", "-sample_rate", "48000",
            "-channels", "2", "-audio_buffer_size", "40", "-i", source,
            "-t", "18", "-c", "copy", str(args.directory / "hdmi-capture.mkv"),
        ], stderr=errors)
    time.sleep(1.2)
    return capture


def export_edid(console, directory):
    out = checked(console, f"echo EDID_BEGIN; hexdump -v -e '16/1 \"%02x \" \"\\n\"' {PROBE}/edid && "
                  f"echo EDID_END && sha256sum {PROBE}/edid", 35)
    match = re.search(rb"(?ms)^EDID_BEGIN\r?\n([0-9a-f \r\n]*)^EDID_END\r?$", out)
    if not match:
        raise RuntimeError("missing serial EDID delimiters")
    edid = bytes.fromhex(match[1].decode())
    if not edid:
        return False
    (directory / "edid.bin").write_bytes(edid)
    (directory / "edid.hex").write_text(
        "".join(f"{i:04x}  {edid[i:i+16].hex(' ')}\n" for i in range(0, len(edid), 16)),
        encoding="ascii",
    )
    digest = hashlib.sha256(edid).hexdigest().encode()
    if digest + b"  " + f"{PROBE}/edid".encode() not in out:
        raise RuntimeError("serial EDID differs from the debugfs blob")
    if len(edid) % 128 or any(sum(edid[i:i+128]) % 256 for i in range(0, len(edid), 128)):
        raise RuntimeError("bad EDID block length or checksum")
    console.record(f"[host] Exported {len(edid) // 128} EDID blocks, SHA256 {digest.decode()}\n".encode())
    return True


def exercise(console, args):
    checked(console, "echo N > /sys/module/printk/parameters/ignore_loglevel && dmesg -n 3")
    stage(console, args, ARTIFACTS / "arm64-7.3.6-usb-full", AUDIO)
    stage(console, args, args.directory, ("vblank-check",))
    checked(console, f"chmod 755 /tmp/vdp-player /tmp/ld-musl-aarch64.so.1 /tmp/vblank-check && "
                     f"cat {PROBE}/report && fb-test")
    checked(console, "(/tmp/vblank-check >/tmp/probe-vblank.txt 2>&1 &); "
                     "sleep 1; grep '^VT1 ready$' /tmp/probe-vblank.txt")
    initial = checked(console, SNAPSHOT)
    capture = record_capture(args)
    failures = []
    try:
        checked(console, "cd /tmp && (./ld-musl-aarch64.so.1 --library-path . "
                "./vdp-player --tone 12 >/tmp/probe-tone.txt 2>&1 & echo $! >/tmp/probe-tone.pid)")
        checked(console, "sleep 1; cat /proc/asound/card0/pcm0p/sub0/status")
        before = checked(console, SNAPSHOT)
        for command in ("hpd", "edid", "cec"):
            rc, out = shell(console, f"echo {command} > {PROBE}/run; rc=$?; "
                            f"cat {PROBE}/report; (exit $rc)", 80)
            (args.directory / f"{command}.txt").write_bytes(out)
            if rc:
                failures.append(command)
        after = checked(console, SNAPSHOT)
        tail = checked(console, "while kill -0 $(cat /tmp/probe-tone.pid) 2>/dev/null && "
                       "! grep -q 'ALSA played=' /tmp/probe-tone.txt; do sleep 1; done; "
                       "cat /tmp/probe-tone.txt; cat /proc/asound/card0/pcm0p/sub0/status", 22)
        final = checked(console, SNAPSHOT)
        vt1 = checked(console, "sleep 5; cat /tmp/probe-vblank.txt")
    finally:
        if capture:
            try:
                capture.wait(timeout=20)
            except subprocess.TimeoutExpired:
                capture.kill()
                capture.wait()
            if capture.returncode:
                failures.append("capture")

    counters = initial + before + after + final
    vblank = list(map(int, re.findall(rb"(?m)^vblank_irqs (\d+) ", counters)))
    thresholds = re.findall(rb"(?m)^c228 ([0-9a-f]+)\r?$", counters)
    aio = count_irq(counters, 116)
    if (count_irq(counters, 181) != [0] * 4 or len(vblank) != 4 or
            any(b <= a for a, b in zip(vblank, vblank[1:])) or
            len(thresholds) != 4 or len(set(thresholds)) != 1 or
            counters.count(b"underflow_irqs 0\r\n") != 4 or
            counters.count(b"fetch_underflow 00000000\r\n") != 4 or b"VT1 waits=1080" not in vt1):
        failures.append("VT1/underflow/SMMU")
    if (b"ALSA played=576000 frames rate=48000 xruns=0" not in tail or
            not re.search(rb"(?m)^closed\r?$", tail) or len(aio) != 4 or aio[-1] <= aio[0]):
        failures.append("AIAO")
    if not export_edid(console, args.directory):
        failures.append("EDID export")
    out = checked(console, "dmesg", 20)
    if re.search(rb"SError|external abort|Kernel panic|SMMU faults?=[1-9]|"
                 rb"BUG:|WARNING:|disabling IRQ|nobody cared", out, re.I):
        failures.append("kernel fault")
    checked(console, "cat /proc/mounts /proc/partitions")
    console.record(f"[host] VT1 {vblank}, AIAO IRQ {aio}, SMMU 181 {count_irq(counters, 181)}\n".encode())
    console.record(f"[host] Failed checks: {', '.join(failures) or 'none'}\n".encode())
    return int(bool(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--host", type=IPv4Address, default="192.168.6.172")
    parser.add_argument("--peer", type=IPv4Address, default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--capture-video")
    parser.add_argument("--capture-audio")
    parser.add_argument("--running-ram", action="store_true")
    args = parser.parse_args()
    args.host = str(args.host)
    args.peer = str(args.peer)
    port = PacedSerial(baudrate=115200, bytesize=8, parity="N", stopbits=1,
                       timeout=.08, write_timeout=3)
    port.port = "COM4"
    port.dtr = port.rts = False
    with port, (args.directory / "board-com4.txt").open("ab" if args.running_ram else "wb") as log:
        console = LogConsole(port, log)
        if not args.running_ram:
            ram_boot(console, args)
        verify_boot(console, RELEASE)
        checked(console, f"cat {CURSOR} > /tmp/probe-cursor.txt && echo 0 > {CURSOR}")
        try:
            return exercise(console, args)
        finally:
            checked(console, f"cat /tmp/probe-cursor.txt > {CURSOR}")


if __name__ == "__main__":
    raise SystemExit(main())
