"""Load FFmpeg and video samples into the HG680KD RAM system over HTTP."""

import argparse
import hashlib
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from ipaddress import IPv4Address
from pathlib import Path
import re
from threading import Thread

import serial

from serial_boot import Console, SHELL_PROMPT
from vdec_ram_test import boot


CLIPS = {
    "h264-1080p": "nv21",
    "h264-1080p-no-direct": "nv21",
    "hevc-1080p": "nv21",
    "hevc-2160p": "nv21",
    "hevc10-2160p": "p010le",
}
BINARIES = ("ffmpeg", "ld-musl-aarch64.so.1", "libdrm.so.2", "libudev.so.1")
RAM_DIR = "/tmp"
FRAMES = 240


def shell(console, command, timeout=360):
    console.port.write(f"({command}); printf '\\nEXIT=%s\\n' $?\r".encode("ascii"))
    output = console.expect(SHELL_PROMPT, timeout)
    match = re.search(rb"(?m)^EXIT=(\d+)\r?$", output)
    if not match:
        raise RuntimeError(f"missing exit status for {command}")
    return int(match[1]), output


class PayloadHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, directory, peer):
        self.peer = str(peer)
        super().__init__(*args, directory=directory)

    def do_GET(self):
        if self.client_address[0] != self.peer:
            self.send_error(403)
            return
        super().do_GET()


def load_payload(console, args, clips):
    names = (*BINARIES, "vdec_ffmpeg_board.sh", *(f"{clip}.mp4" for clip in clips))
    expected = {
        name: hashlib.sha256((args.payload / name).read_bytes()).hexdigest()
        for name in names
    }
    handler = partial(PayloadHandler, directory=args.payload, peer=args.peer)
    with ThreadingHTTPServer((str(args.server), args.http_port), handler) as httpd:
        thread = Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            if shell(console, f"ifconfig eth0 {args.peer} netmask 255.255.255.0 up")[0]:
                raise RuntimeError("cannot configure board Ethernet")
            for name, digest in expected.items():
                status, log = shell(console, f"cd {RAM_DIR} && wget -q -O {name} "
                                    f"http://{args.server}:{args.http_port}/{name} && "
                                    f"sha256sum {name}", timeout=120)
                if status or f"{digest}  {name}".encode() not in log:
                    raise RuntimeError(f"payload verification failed for {name}")
            if shell(console, f"cd {RAM_DIR} && chmod 755 ffmpeg ld-musl-aarch64.so.1")[0]:
                raise RuntimeError("cannot make the executable payload runnable")
        finally:
            httpd.shutdown()
            thread.join()


def interrupts_ok(output):
    counters = {}
    for irq in (169, 181):
        counts = re.findall(rb"(?m)^ *\d+: +([\d ]+)GICv3 +" + str(irq).encode() + b" ", output)
        counters[irq] = [sum(map(int, line.split())) for line in counts]
    return (len(counters[169]) == 2 and counters[169][1] - counters[169][0] == FRAMES
            and counters[181] == [0, 0])


def check_pixels(console, runner, clip, references):
    status, output = shell(console, f"{runner} check {clip} {CLIPS[clip]}")
    reference = (references / f"{clip}.sw.framemd5").read_bytes().splitlines()
    hashes = [line.split(b",")[5] for line in reference if line and not line.startswith(b"#")]
    expected = hashlib.sha256(b"\n".join(hashes) + b"\n").hexdigest().encode()
    matched = (status == 0 and len(hashes) == FRAMES
               and re.search(rb"(?m)^HASH FRAMES 240\r?$", output)
               and re.search(rb"(?m)^" + expected + rb"  -\r?$", output)
               and interrupts_ok(output))
    console.record(f"\n[host] {clip} pixels {'MATCH' if matched else 'FAILED'}\n".encode())
    return bool(matched)


def check_performance(status, output):
    frames = re.findall(rb"(?m)^frame=(\d+)\r?$", output)
    timings = re.findall(rb"(?m)^TIMING (?:BEFORE|AFTER) ([^\r\n]*)", output)
    if status or not frames or len(timings) != 2 or not interrupts_ok(output):
        return False
    before, after = timings
    decoded = re.search(rb"timing frames=(\d+)", after)
    return bool(decoded and before != after and int(frames[-1]) == int(decoded[1]) == FRAMES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--references", type=Path, help="software framemd5 directory")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--server", type=IPv4Address, default="192.168.6.172")
    parser.add_argument("--peer", type=IPv4Address, default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--mode", choices=("probe", "check", "perf"), required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("clips", nargs="*")
    args = parser.parse_args()
    clips = args.clips or [clip for clip in CLIPS if clip != "h264-1080p-no-direct"]
    references = args.references or args.payload
    if args.runs < 1 or any(clip not in CLIPS for clip in clips):
        parser.error("supply a positive run count and known clip names")
    if args.mode == "check" and not references:
        parser.error("--mode check needs --references or --payload for pixel comparison")

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
        if (status or b"rootfs / rootfs " not in mounts or
                not re.search(rb"(?m)^EMMC_RO=1\r?$", mounts) or
                re.search(rb"(?m)^/dev/\S+ ", mounts)):
            raise RuntimeError("expected RAM root and read-only eMMC with no mounted block devices")
        shell(console, "echo N > /sys/module/printk/parameters/ignore_loglevel; dmesg -n 3")
        if args.payload:
            load_payload(console, args, () if args.mode == "probe" else clips)

        runner = f"sh {RAM_DIR}/vdec_ffmpeg_board.sh"
        if args.mode in ("probe", "check"):
            status, output = shell(console, f"{runner} probe")
            if status or not re.search(rb"(?m)^v4l2request\r?$", output):
                raise RuntimeError("board FFmpeg is missing v4l2request")
            if args.mode == "probe":
                return 0
        for clip in clips:
            if args.mode == "check":
                passed &= check_pixels(console, runner, clip, references)
            else:
                for run in range(1, args.runs + 1):
                    status, output = shell(console, f"{runner} perf {clip} {run}")
                    valid = check_performance(status, output)
                    console.record(f"\n[host] {clip} run {run} {'VALID' if valid else 'FAILED'}\n".encode())
                    passed &= valid
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
