"""Play MP4/MKV files on the HG680KD in the Phase 2 TFTP RAM system."""

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
from vdec_ram_test import boot
from vdp_ram_test import IRQ, SCANOUT, count_irq


FILES = ("main10-2160p60.mkv", "main-2160p60.mp4", "h264-1080p60.mp4")
BINARIES = ("vdp-player", "ld-musl-aarch64.so.1", "libdrm.so.2", "libudev.so.1")
RAM = "/tmp"
PLAYER = "./ld-musl-aarch64.so.1 --library-path . ./vdp-player"
SNAPSHOT = ("grep -E '^(MemAvailable|Shmem|CmaTotal|CmaFree):' /proc/meminfo; " + IRQ + "; "
            f"grep -E '^(vblank_irqs|fetch_underflow|0208|2004|2018|2088|208c|22c0|22dc|22e8|"
            f"5004|5018|8240|8254|825c|8284|828c|8294|8338|8a08|8a54|8b38|b000|b050)' {SCANOUT}")


def load_payload(console, args):
    handler = partial(PayloadHandler, directory=args.payload, peer=args.peer)
    with ThreadingHTTPServer((str(args.server), args.http_port), handler) as httpd:
        worker = Thread(target=httpd.serve_forever, daemon=True)
        worker.start()
        try:
            status, _ = shell(console, f"ifconfig eth0 {args.peer} netmask 255.255.255.0 up")
            if status:
                raise RuntimeError("cannot configure board Ethernet")
            for name in (*BINARIES, *FILES, *(("color-switch.mkv",) if args.color_switch else ())):
                with (args.payload / name).open("rb") as data:
                    digest = hashlib.file_digest(data, "sha256").hexdigest()
                status, output = shell(console, f"cd {RAM} && wget -q -O {name} "
                                       f"http://{args.server}:{args.http_port}/{name} && "
                                       f"sha256sum {name}", timeout=120)
                if status or f"{digest}  {name}".encode() not in output:
                    raise RuntimeError(f"payload verification failed for {name}")
            status, _ = shell(console, f"cd {RAM} && chmod 755 vdp-player ld-musl-aarch64.so.1")
            if status:
                raise RuntimeError("cannot mark player and ELF loader as executable")
        finally:
            httpd.shutdown()
            worker.join()


def capture_hdmi(args, label, still):
    if not args.capture_device:
        return None
    directory = args.log.parent
    output = (["-vf", r"select=gte(t\,4)", "-frames:v", "1", "-threads", "2",
               "-update", "1", str(directory / f"{label}.png")] if still else
              ["-t", "10", "-c:v", "copy", str(directory / f"{label}.mkv")])
    errors = (directory / f"{label}.capture.txt").open("wb")
    with errors:
        capture = subprocess.Popen([
            "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y", "-f", "dshow",
            "-rtbufsize", "64M", "-video_size", "1920x1080", "-framerate", "30", "-vcodec", "mjpeg",
            "-i", f"video={args.capture_device}", *output,
        ], stderr=errors)
    time.sleep(0.8)
    return capture


def run(console, args, label, filename, expected=None, options="", seeks=0, capture=False,
        still=False, color_switch=False):
    camera = capture_hdmi(args, label, still) if capture else None
    command = " ".join(part for part in (PLAYER, options, filename) if part)
    extra_snapshots = ("sleep 2; snap; sleep 2; snap; sleep 2; snap; " if color_switch else
                       "sleep 8; snap; " if expected else "")
    console.record(f"\n[host] {label}: cd {RAM} && {command}\n".encode())
    try:
        status, output = shell(console, f"cd {RAM} && {{ snap() {{ {SNAPSHOT}; }}; echo BEFORE; snap; "
                               "printf 'TIMINGS_BEFORE='; dmesg | grep -c 'timing frames='; "
                               f"{command} > playback.log 2>&1 & p=$!; sleep 1; echo DURING; "
                               f"snap; {extra_snapshots}"
                               "wait $p; result=$?; cat playback.log; sleep 1; echo AFTER; "
                               "snap; dmesg | grep -E 'timing frames=|SMMU fault|underflow'; "
                               "(exit $result); }", timeout=75)
    finally:
        if camera:
            try:
                camera.wait(timeout=15)
            except subprocess.TimeoutExpired:
                camera.kill()
                camera.wait()
    vdp = count_irq(output, 122)
    pxp = count_irq(output, 169)
    smmu = count_irq(output, 181)
    baseline = re.search(rb"(?m)^TIMINGS_BEFORE=(\d+)", output)
    timing = re.findall(rb"timing frames=(\d+).*?detile=(\d+).*?sync=(\d+)", output)
    cma = list(map(int, re.findall(rb"(?m)^CmaFree:\s+(\d+) kB", output)))
    result = re.search(rb"PLAY decoded=(\d+) displayed=(\d+) dropped=(\d+) preroll=(\d+) "
                       rb"seeks=(\d+) eos=(\d+) missed_vblanks=(\d+) fps=([\d.]+)", output)
    if len(vdp) < 2 or len(pxp) < 2 or not result or not baseline or not timing:
        console.record(f"[host] {label} FAIL missing counters or player result\n".encode())
        return False
    vdp_delta, pxp_delta = vdp[-1] - vdp[0], pxp[-1] - pxp[0]
    timing = timing[int(baseline[1]):]
    smmu_delta = smmu[-1] - smmu[0] if len(smmu) >= 2 else -1
    shown = int(result[2])
    valid = (not status and 0 < shown <= pxp_delta and vdp_delta >= shown and
             len(cma) >= 2 and cma[0] - cma[-1] <= 4096 and
             len(smmu) >= 2 and not any(smmu) and not re.search(rb"underflow_irqs [1-9]", output) and
             b"underflow_irqs 0" in output and b"fetch_underflow 00000000" in output and
             not re.search(rb"fetch_underflow (?!00000000)[0-9a-f]+", output) and
             sum(int(row[0]) for row in timing) == pxp_delta and
             all(row[1:] == (b"0", b"0") for row in timing) and
             int(result[5]) == seeks and (not camera or camera.returncode == 0))
    if seeks:
        valid &= result[6] == b"1" and int(result[3]) == 0
    if expected:
        valid &= (tuple(map(int, result.groups()[:6])) == (expected, expected, 0, 0, 0, 1) and
                  int(result[7]) == 0 and float(result[8]) >= 59.9)
        if "--osd" in options:
            progress = re.search(rb"(?m)^G0 progress flips=(\d+) permille=(\d+)", output)
            valid &= bool(progress and int(progress[1]) > 10 and int(progress[2]) >= 980)
    if color_switch:
        properties = re.findall(rb"(?m)^V0 color COLOR_ENCODING=(\d+) COLOR_RANGE=(\d+)", output)
        csc = re.findall(rb"(?m)^b000 ([0-9a-f]+)\r?\nb050 ([0-9a-f]+)", output)[1:-1]
        valid &= (properties == [(b"1", b"0"), (b"0", b"1"), (b"2", b"0"), (b"1", b"0")] and
                  len(csc) == 4 and csc[0][0] == csc[3][0] == b"00000002" and
                  csc[1:3] == [(b"00000013", b"00000000"), (b"00000013", b"000007c0")])
    returned = cma[-1] - cma[0] if len(cma) >= 2 else None
    console.record(f"[host] {label} VDP IRQ +{vdp_delta}, PXP IRQ +{pxp_delta}, "
                   f"SMMU 181 +{smmu_delta}, CMA free change {returned} KiB "
                   f"{'PASS' if valid else 'FAIL'}\n".encode())
    return bool(valid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--image", type=Path, help="TFTP boot this Phase 2 RAM image")
    source.add_argument("--running-ram", action="store_true",
                        help="reuse the Phase 2 image already TFTP-booted on the target")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--server", type=IPv4Address, default="192.168.6.172")
    parser.add_argument("--peer", type=IPv4Address, default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--capture-device", help="DirectShow HDMI capture device")
    parser.add_argument("--color-switch", action="store_true", help="also run color-switch.mkv")
    parser.add_argument("--smoke", action="store_true", help="run only a short Main10 playback")
    args = parser.parse_args()

    port = serial.Serial(baudrate=115200, bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
                         stopbits=serial.STOPBITS_ONE, timeout=.08, write_timeout=3)
    port.port = args.port
    port.dtr = port.rts = False
    with port, args.log.open("wb") as logfile:
        console = Console(port, logfile)
        if args.image:
            boot(console, args.image, args.server, args.peer, args.log.with_suffix(".tftp.txt"))
        else:
            port.write(b"\r")
            console.expect(SHELL_PROMPT)
        status, output = shell(console, "uname -r; cat /proc/mounts; "
                               "printf EMMC_RO=; cat /sys/block/mmcblk0/ro")
        if (status or b"7.3.5-hg680kd-arm64-vdp" not in output or
                not re.search(rb"(?m)^rootfs / rootfs ", output) or
                not re.search(rb"(?m)^EMMC_RO=1\r?$", output) or
                re.search(rb"(?m)^/dev/\S+ ", output)):
            raise RuntimeError("expected a VDP RAM boot with read-only eMMC and no block mounts")
        shell(console, "echo N > /sys/module/printk/parameters/ignore_loglevel; dmesg -n 3")
        load_payload(console, args)
        if args.smoke:
            return 0 if run(console, args, "main10-smoke", FILES[0], options="--stop-at 2") else 1
        passed = run(console, args, "main10-eos", FILES[0], expected=1080)
        passed &= run(console, args, "main-eos", FILES[1], expected=1080)
        passed &= run(console, args, "h264-osd-playback", FILES[2], expected=1080,
                      options="--osd", capture=True)
        passed &= run(console, args, "h264-osd-held", FILES[2], options="--osd --hold-first 8",
                      capture=True, still=True)
        passed &= run(console, args, "h264-clean-held", FILES[2], options="--hold-first 8",
                      capture=True, still=True)
        passed &= run(console, args, "main10-seek", FILES[0],
                      options="--seek 2:9 --seek 11:3 --seek 5:16", seeks=3)
        for i in range(3):
            passed &= run(console, args, f"main10-restart-{i + 1}", FILES[0],
                          options="--start 2 --stop-at 4")
        if args.color_switch:
            passed &= run(console, args, "color-switch", "color-switch.mkv", expected=480,
                          color_switch=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
