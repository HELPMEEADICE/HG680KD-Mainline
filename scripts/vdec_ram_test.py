"""Boot a decoder image via TFTP into RAM and compare board NV21 frame hashes."""

import argparse
import hashlib
from ipaddress import IPv4Address
from pathlib import Path
import re
import subprocess
import sys
import time

import serial

from serial_boot import Console, PROMPT, SHELL_PROMPT


LOAD_ADDRESS = 0x01FDFFC0
HASH = re.compile(rb"(?m)^([a-f0-9]{64})  -\r?$")
SMMU_IRQ = re.compile(rb"(?m)^\s*\d+:\s+([\d \t]+)GICv3\s+181\b")


def boot(console, image, server, peer, tftp_log):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", image.name):
        raise ValueError("image filename must be a single U-Boot argument")
    console.port.write(b"\r")
    prompt = console.expect(re.compile(re.escape(PROMPT) + b"|" + re.escape(SHELL_PROMPT)))
    if prompt.endswith(SHELL_PROMPT):
        console.port.write(b"reboot -f\r")
        console.watch_uboot(60)
    with tftp_log.open("wb") as output:
        tftp = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("tftp_send.py")),
             str(image), "--bind", str(server), "--peer", str(peer)],
            stdout=output, stderr=subprocess.STDOUT,
        )
        try:
            deadline = time.monotonic() + 5
            while not tftp_log.read_bytes().startswith(b"Listening on UDP"):
                if tftp.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"TFTP server did not start, see {tftp_log}")
                time.sleep(0.05)
            for command in (
                f"setenv ipaddr {peer}", f"setenv serverip {server}", "setenv tftpdstp 1069",
                "mmc read 0 0x8020000 0x10000 0x1000",
                f"tftpboot {LOAD_ADDRESS:x} {image.name}",
            ):
                console.command(command, timeout=30)
            data = image.read_bytes()
            console.record(f"\n[host] boot.img SHA256 {hashlib.sha256(data).hexdigest()}\n".encode())
            console.verify(data, LOAD_ADDRESS)
        finally:
            tftp.terminate()
            tftp.wait(timeout=5)
    console.port.write(f"bootm {LOAD_ADDRESS:x}\r".encode())
    console.expect(SHELL_PROMPT, timeout=60)


def shell(console, command):
    console.port.write(command.encode("ascii") + b"\r")
    return console.expect(SHELL_PROMPT, timeout=60)


def report(console, name, hashes, reference, frames):
    raw = reference.read_bytes()
    size, remainder = divmod(len(raw), frames)
    if not size or remainder:
        raise ValueError(f"invalid reference size for {name}")
    matches = 0
    for i in range(frames):
        expected = hashlib.sha256(raw[i * size:(i + 1) * size]).hexdigest()
        actual = hashes[i].decode() if i < len(hashes) else "missing"
        result = "MATCH" if actual == expected else "MISMATCH"
        matches += actual == expected
        console.record(f"[host] {name} frame {i} board={actual} FFmpeg={expected} {result}\n".encode())
    console.record(f"[host] RESULT {name} {matches}/{frames}\n".encode())
    return matches == frames and len(hashes) == frames


def compare_fixture(console, fixture, visible_height):
    header = fixture.with_suffix(".txt").read_text(encoding="ascii").splitlines()[0].split()
    hevc = header[0] == "HEVC"
    width, height, frames = map(int, header[int(hevc):int(hevc) + 3])
    size = width * height * 3 // 2
    name = fixture.name
    if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
        raise ValueError(f"invalid fixture name {name!r}")
    code = 265 if hevc else 264
    output = shell(console, f"echo TEST-{name}; "
                   f"vdec-request /dev/video0 /dev/media0 /etc/vdec-test/{name}.txt "
                   f"/etc/vdec-test/{name}.{code} /tmp/vdec-check.nv21 && "
                   f"for n in {' '.join(map(str, range(frames)))}; do "
                   f"dd if=/tmp/vdec-check.nv21 bs={size} skip=$n count=1 2>/dev/null | sha256sum; done; "
                   "grep hi3798mv320-vdec /proc/interrupts")
    passed = report(console, name, HASH.findall(output), fixture.with_suffix(".nv21"), frames)
    if visible_height and passed:
        hashes = []
        if not 0 < visible_height <= height:
            raise ValueError(f"invalid visible height for {name}")
        for i in range(frames):
            start = i * height * 3 // 2
            hashes.extend(HASH.findall(shell(
                console, f"{{ dd if=/tmp/vdec-check.nv21 bs={width} skip={start} count={visible_height}; "
                f"dd if=/tmp/vdec-check.nv21 bs={width} skip={start + height} count={visible_height // 2}; "
                "} 2>/dev/null | sha256sum",
            )))
        passed &= report(console, f"{name}-visible", hashes,
                         fixture.with_suffix(".visible.nv21"), frames)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--server", type=IPv4Address, default="192.168.6.172")
    parser.add_argument("--peer", type=IPv4Address, default="192.168.6.249")
    parser.add_argument("--crop", action="append", default=[], help="fixture_name=visible_height")
    parser.add_argument("fixtures", type=Path, nargs="+", help="local fixture prefixes")
    args = parser.parse_args()
    crops = dict(item.split("=", 1) for item in args.crop)

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
        shell(console, "uname -r; cat /proc/mounts; cat /sys/block/mmcblk0/ro; "
              "grep hi3798mv320-vdec /proc/interrupts")
        shell(console, "echo N > /sys/module/printk/parameters/ignore_loglevel; dmesg -n 3")
        for fixture in args.fixtures:
            passed &= compare_fixture(console, fixture, int(crops.get(fixture.name, 0)))
        status = shell(console, "grep hi3798mv320-vdec /proc/interrupts; "
                       "dmesg | grep -E 'PXP 0x|SMMU fault|decode returned'")
        smmu = SMMU_IRQ.search(status)
        passed &= smmu is not None and not any(map(int, smmu[1].split()))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
