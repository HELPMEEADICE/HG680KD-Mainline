"""Check VDP V0 scanout on the HG680KD after a TFTP RAM boot."""

import argparse
import hashlib
from functools import partial
from http.server import ThreadingHTTPServer
from ipaddress import IPv4Address
from pathlib import Path
import re
import subprocess
from threading import Thread
import time

import serial

from serial_boot import Console, SHELL_PROMPT
from vdec_ffmpeg_ram import PayloadHandler, shell
from vdec_ram_test import boot, compare_fixture


BUILTIN = ("h265ip1080", "hevc10ip1080", "hevc4k", "hevc10ip4k")
IRQ = "grep -E 'GICv3 +(122|169|181) ' /proc/interrupts"
SCANOUT = "/sys/bus/platform/devices/f8cc0000.display-controller/scanout"


def count_irq(output, intid):
    return [sum(map(int, counters.split())) for counters in re.findall(
        rb"(?m)^\s*\d+: +([\d ]+)GICv3 +" + str(intid).encode() + rb" ", output)]


def download_fixtures(console, args, names):
    handler = partial(PayloadHandler, directory=args.payload, peer=args.peer)
    with ThreadingHTTPServer((str(args.server), args.http_port), handler) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            if shell(console, f"ifconfig eth0 {args.peer} netmask 255.255.255.0 up")[0]:
                raise RuntimeError("cannot configure board Ethernet")
            for name in names:
                for ext in ("txt", "265"):
                    filename = f"{name}.{ext}"
                    with (args.payload / filename).open("rb") as source:
                        digest = hashlib.file_digest(source, "sha256").hexdigest()
                    status, output = shell(console, f"cd /tmp && wget -q -O {filename} "
                                           f"http://{args.server}:{args.http_port}/{filename} && "
                                           f"sha256sum {filename}", timeout=120)
                    if status or f"{digest}  {filename}".encode() not in output:
                        raise RuntimeError(f"fixture download failed for {filename}")
        finally:
            server.shutdown()
            thread.join()


def request(prefix):
    return f"vdec-request /dev/video0 /dev/media0 {prefix}.txt {prefix}.265 --display /dev/dri/card0"


def camera_rgb(path, crop):
    filters = "scale=160:90:flags=area"
    if crop:
        filters = "crop=iw/2:ih/2:iw/4:ih/4," + filters
    return subprocess.check_output([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(path), "-vf", filters,
        "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-threads", "1", "-",
    ], timeout=8)


def check_frame(console, args, name):
    command = (f"{request(f'/etc/vdec-test/{name}')} 1 9000 & p=$!; "
               f"sleep 1; {IRQ}; cat {SCANOUT}; sleep 1; {IRQ}; "
               "printf '\\nFRAME_HELD\\n'; wait $p; printf '\\nFRAME_EXIT=%s\\n' $?")
    camera = None
    if args.capture_device:
        capture = args.log.parent / f"{name}-hdmi.png"
        camera = subprocess.Popen([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "dshow",
            "-video_size", "1920x1080", "-framerate", "30", "-vcodec", "mjpeg",
            "-i", f"video={args.capture_device}", "-vf", r"select=gte(t\,4)",
            "-frames:v", "1", "-threads", "2", "-update", "1", str(capture),
        ])
        time.sleep(0.8)
    try:
        console.port.write(command.encode("ascii") + b"\r")
        first = console.expect(b"\r\nFRAME_HELD\r\n", timeout=20)
        last = console.expect(SHELL_PROMPT, timeout=20)
    finally:
        if camera:
            try:
                camera.wait(timeout=1)
            except subprocess.TimeoutExpired:
                camera.kill()
                camera.wait()
    regs = {int(reg, 16): int(value, 16) for reg, value in re.findall(
        rb"(?m)^([0-9a-f]{4}) ([0-9a-f]{8})\r?$", first)}
    counts = count_irq(first, 122)
    blank = re.search(rb"vblank_irqs (\d+) in_blank (\d+)", first)
    main10 = name.startswith("hevc10")
    fb = bool(b"V0 complete=1 " in first and b"FRAME_EXIT=0\r\n" in last and
              len(counts) == 2 and 59 <= counts[1] - counts[0] <= 62 and
              blank and int(blank[2]) >= int(blank[1]) - 1 and
              count_irq(first, 181) == [0, 0] and regs.get(0x2004) == 0 and
              regs.get(0x2018, 0) & (1 << 31) and regs.get(0x8220, 0) & 3 == 3 and
              regs.get(0x8240) == (0x10a if main10 else 0xa) and
              regs.get(0x8254, 0) == regs.get(0x8338) and regs.get(0x825c, 0) and
              all(bool(regs.get(reg, 0)) == main10 for reg in (0x8284, 0x828c, 0x8294)) and
              b"unexpected_status 00000000" in first)
    visual = not camera or camera.returncode == 0
    if visual and args.capture_reference:
        crop = name.endswith("1080") and b"firmware=3840x2160" in first
        measured = camera_rgb(capture, crop)
        expected = camera_rgb(args.capture_reference / f"{name}-reference.png", False)
        if len(measured) != 160 * 90 * 3 or len(measured) != len(expected):
            raise RuntimeError("unexpected HDMI comparison image size")
        difference = sum(abs(a - b) for a, b in zip(measured, expected)) / len(expected)
        visual = difference <= 24
        console.record(f"\n[host] {name} HDMI reference MAE={difference:.2f}/255 "
                       f"{'PASS' if visual else 'FAIL'}\n".encode())
    return fb, visual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--server", type=IPv4Address, default="192.168.6.172")
    parser.add_argument("--peer", type=IPv4Address, default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--payload", type=Path, help="local directory holding extra HEVC fixtures")
    parser.add_argument("--capture-device", help="DirectShow HDMI capture device name")
    parser.add_argument("--capture-reference", type=Path,
                        help="directory holding FFmpeg first-frame reference PNGs")
    parser.add_argument("--linear", type=Path, action="append", default=[],
                        help="fixture prefix with a local NV21/P010 reference")
    parser.add_argument("fixtures", nargs="*", help="extra HEVC fixtures to download and flip")
    args = parser.parse_args()
    if args.fixtures and (not args.payload or any(not re.fullmatch(r"[A-Za-z0-9_-]+", name)
                                                for name in args.fixtures)):
        parser.error("extra fixtures require a payload directory and plain fixture names")
    if args.capture_reference and not args.capture_device:
        parser.error("capture comparison requires a capture device")

    port = serial.Serial(baudrate=115200, timeout=.08, write_timeout=3)
    port.port = args.port
    port.dtr = port.rts = False
    passed = True
    with port, args.log.open("wb") as log:
        console = Console(port, log)
        if args.image:
            boot(console, args.image, args.server, args.peer, args.log.with_suffix(".tftp.txt"))
        else:
            port.write(b"\r")
            console.expect(SHELL_PROMPT)
        status, mounts = shell(console, "uname -r; cat /proc/mounts; "
                               "printf 'EMMC_RO='; cat /sys/block/mmcblk0/ro")
        if (status or b"7.3.5-hg680kd-arm64-vdp" not in mounts or b"rootfs / rootfs " not in mounts or
                not re.search(rb"(?m)^EMMC_RO=1\r?$", mounts) or re.search(rb"(?m)^/dev/\S+ ", mounts)):
            raise RuntimeError("expected VDP RAM image with read-only eMMC and no mounted block devices")
        shell(console, "echo N > /sys/module/printk/parameters/ignore_loglevel; dmesg -n 3")
        status, initial = shell(console, f"ls -l /dev/dri/card0 /dev/video0 /dev/media0 && {IRQ}")
        passed &= status == 0 and count_irq(initial, 181) == [0]
        for name in BUILTIN:
            valid, visual = check_frame(console, args, name)
            console.record(f"\n[host] {name} single-frame FB and IRQ "
                           f"{'PASS' if valid else 'FAIL'}\n".encode())
            passed &= valid and visual
        if args.fixtures:
            download_fixtures(console, args, args.fixtures)
        for prefix in (*[f"/etc/vdec-test/{name}" for name in BUILTIN],
                       *[f"/tmp/{name}" for name in args.fixtures]):
            status, output = shell(console, f"{request(prefix)} && {IRQ}")
            result = re.search(rb"(?m)^V0 result frames=(\d+).* requeued=(\d+) ", output)
            valid = (status == 0 and result and int(result[1]) > 1 and int(result[2]) > 0 and
                     count_irq(output, 181) == [0])
            console.record(f"\n[host] {prefix} continuous {'PASS' if valid else 'FAIL'}\n".encode())
            passed &= bool(valid)
        for fixture in args.linear:
            passed &= compare_fixture(console, fixture, 0)
        _, final = shell(console, f"{IRQ}; cat {SCANOUT}; "
                         "dmesg | grep -Ei 'vdp|underflow|smmu|timing|WARNING|Oops'")
        passed &= (count_irq(final, 181) == [0] and
                   b"unexpected_status 00000000" in final and
                   not re.search(rb"(?im)^<\d+>.*(?:WARNING:|Oops:|underflow|SMMU fault|\*ERROR\*)", final))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
