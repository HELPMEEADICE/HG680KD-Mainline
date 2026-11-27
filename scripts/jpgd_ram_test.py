"""Build JPEG fixtures and validate the HG680KD JPGD RAM-only image over COM4."""

import argparse
from binascii import crc32
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from hashlib import file_digest, sha256
from http.server import ThreadingHTTPServer
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
from threading import Thread

import serial

from serial_boot import Console, PROMPT, SHELL_PROMPT
from tftp_send import serve
from vdec_ffmpeg_ram import PayloadHandler, shell
from vdp_ram_test import count_irq


ROOT = Path(__file__).resolve().parents[1]
RELEASE = b"7.3.6-hg680kd-arm64-jpgd"
FRAMES = 120
SAMPLES = (("720p", 1280, 720), ("1080p", 1920, 1080), ("2160p", 3840, 2160))
AUDIO = ("vdp-player", "ld-musl-aarch64.so.1", "libdrm.so.2", "libudev.so.1")
IRQ = "grep -E 'GICv3 +(116|122|129|169|181) ' /proc/interrupts"
FAULT = re.compile(rb"SError|external abort|Kernel panic|SMMU (fault|error)|"
                   rb"decode timed out|deprecated workqueue|WARNING:|BUG:", re.I)


class LogConsole(Console):
    def record(self, data):
        self.log.write(data)
        if len(data) > 1 or data.endswith(b"\n"):
            self.log.flush()
        if data.startswith(b"[host]"):
            print(data.decode().strip(), flush=True)


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                    *map(str, args)], check=True)


def prepare(directory):
    directory.mkdir(parents=True, exist_ok=True)

    def encode(width, height, frames, output):
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate=30",
               "-frames:v", frames, "-vf", "scale=in_range=tv:out_range=pc,format=yuvj420p",
               "-c:v", "mjpeg", "-threads", 1, "-huffman", "optimal", "-q:v", 3,
               "-f", "image2", output)

    for name, width, height in SAMPLES:
        image = directory / f"{name}.jpg"
        encode(width, height, 1, image)
        ffmpeg("-i", image, "-frames:v", 1, "-pix_fmt", "yuvj420p", "-threads", 1,
               "-f", "rawvideo", directory / f"{name}.ref.i420")
    (directory / "malformed.jpg").write_bytes((directory / "720p.jpg").read_bytes()[:-2])
    with tempfile.TemporaryDirectory() as tmp:
        encode(1920, 1080, FRAMES, Path(tmp) / "%06d.jpg")
        with (directory / "1080p-sequence.bin").open("wb") as packed:
            for n in range(1, FRAMES + 1):
                frame = (Path(tmp) / f"{n:06d}.jpg").read_bytes()
                packed.write(struct.pack(">I", len(frame)))
                packed.write(frame)


def ram_boot(console, args):
    firmware = args.bl31.read_bytes()
    if len(firmware) != 0x40000 or crc32(firmware) != 0xfa236592:
        raise RuntimeError("BL31 must match the accepted stock RAM copy")
    boot_image = (args.directory / "boot.img").read_bytes()
    console.port.write(b"\r")
    prompt = console.expect(re.compile(re.escape(PROMPT) + b"|" + re.escape(SHELL_PROMPT)), 6)
    if prompt.endswith(SHELL_PROMPT):
        status, out = shell(console, "uname -r; cat /proc/mounts", 10)
        if status or b"rootfs / rootfs " not in out or re.search(rb"(?m)^/dev/\S+ ", out):
            raise RuntimeError("Existing serial shell is outside the expected RAM environment")
        console.port.write(b"reboot -f\r")
        console.watch_uboot(60)

    for command in (f"setenv ipaddr {args.peer}", f"setenv serverip {args.host}",
                    "setenv tftpdstp 1069", "setenv autostart no"):
        console.command(command)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener, ThreadPoolExecutor(1) as pool:
        listener.bind((args.host, 1069))
        listener.settimeout(25)
        for name, image, address in (("stock-bl31.bin", firmware, 0x08020000),
                                     ("boot.img", boot_image, 0x01FDFFC0)):
            console.record(f"[host] {name} SHA256 {sha256(image).hexdigest()}\n".encode())
            transfer = pool.submit(serve, listener, name, image, args.peer)
            out = console.command(f"tftpboot {address:x} {name}", timeout=65)
            transfer.result(timeout=5)
            if f"Bytes transferred = {len(image)}".encode() not in out:
                raise RuntimeError(f"Unexpected TFTP size for {name}")
            console.verify(image, address)
    console.port.write(b"bootm 1fdffc0\r")
    console.expect(SHELL_PROMPT, 55)


def checked(console, command, timeout=25):
    status, output = shell(console, command, timeout)
    if status:
        raise RuntimeError(f"Board returned {status} for {command}\n{output.decode(errors='replace')}")
    return output


def stage(console, args, directory, names):
    handler = partial(PayloadHandler, directory=directory, peer=args.peer)
    with ThreadingHTTPServer((args.host, args.http_port), handler) as server:
        worker = Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            checked(console, f"ifconfig eth0 {args.peer} netmask 255.255.255.0 up")
            for name in names:
                with (directory / name).open("rb") as file:
                    digest = file_digest(file, "sha256").hexdigest()
                out = checked(console, f"cd /tmp && wget -q -O {name} "
                              f"http://{args.host}:{args.http_port}/{name} && sha256sum {name}", 40)
                if f"{digest}  {name}".encode() not in out:
                    raise RuntimeError(f"Incorrect board SHA256 for {name}")
        finally:
            server.shutdown()
            worker.join()


def snapshot(console):
    out = checked(console, f"{IRQ}; grep '^Cma' /proc/meminfo; "
                  "cat /sys/kernel/debug/dma_buf/bufinfo")
    jpgd, smmu = count_irq(out, 129), count_irq(out, 181)
    cma = re.search(rb"CmaFree:\s+(\d+)", out)
    dma = re.search(rb"Total\s+(\d+) objects,\s+(\d+) bytes", out)
    if len(jpgd) != 1 or smmu != [0] or not cma or not dma:
        raise RuntimeError("Missing counters or nonzero SMMU INTID 181")
    return jpgd[0], int(cma[1]), tuple(map(int, dma.groups()))


def media_smoke(console):
    out = checked(console, f"{IRQ}; vdec-request /dev/video0 /dev/media0 "
                  "/etc/vdec-test/h265ip1080.txt /etc/vdec-test/h265ip1080.265 "
                  "--display /dev/dri/card0 1 500 && "
                  "(cd /tmp && ./ld-musl-aarch64.so.1 --library-path . ./vdp-player --tone 5) && "
                  f"{IRQ}; cat /proc/asound/card0/pcm0p/sub0/status", 22)
    vdp, pxp, aiao, smmu = (count_irq(out, irq) for irq in (122, 169, 116, 181))
    if (b"V0 complete=1 " not in out or b"ALSA played=240000 frames rate=48000 xruns=0" not in out
            or not re.search(rb"(?m)^closed\r?$", out) or smmu != [0, 0]
            or any(len(c) != 2 or c[1] <= c[0] for c in (vdp, pxp, aiao))):
        raise RuntimeError("VDEC, VDP or AIAO regression")
    console.record(f"[host] VDEC/VDP/AIAO IRQ deltas {pxp[1]-pxp[0]}/{vdp[1]-vdp[0]}/"
                   f"{aiao[1]-aiao[0]}, SMMU INTID 181=0\n".encode())


def receive_frame(listener, peer, path, size):
    with listener.accept()[0] as conn:
        if conn.getpeername()[0] != peer:
            raise RuntimeError("Unexpected YUV sender")
        conn.settimeout(10)
        with path.open("wb") as output:
            received = 0
            while received <= size:
                chunk = conn.recv(min(256 * 1024, size - received + 1))
                if not chunk:
                    break
                output.write(chunk)
                received += len(chunk)
        if received != size:
            raise RuntimeError(f"Expected {size} YUV bytes, received {received}")


def compare(directory, name, width, height, nv21):
    import numpy as np

    fmt = "nv21" if nv21 else "nv12"
    frame = np.fromfile(directory / f"{name}.{fmt}", dtype=np.uint8)
    ref = np.fromfile(directory / f"{name}.ref.i420", dtype=np.uint8)
    pixels = width * height
    hardware = (frame[:pixels], frame[pixels + int(nv21)::2], frame[pixels + int(not nv21)::2])
    reference = (ref[:pixels], ref[pixels:pixels * 5 // 4], ref[pixels * 5 // 4:])
    if frame.size != ref.size or ref.size != pixels * 3 // 2:
        raise RuntimeError(f"{name}: invalid frame size")
    results = []
    for measured, expected in zip(hardware, reference):
        difference = measured.astype(np.float32) - expected
        mse = np.mean(difference ** 2, dtype=np.float64)
        mae = np.mean(np.abs(difference), dtype=np.float64)
        psnr = float("inf") if not mse else 10 * np.log10(255 ** 2 / mse)
        results.append((psnr, mae))
    return results


def single_frame(console, args, listener, test, sample, nv21):
    name, width, height = sample
    fmt = "nv21" if nv21 else "nv12"
    target = args.directory / f"{name}.{fmt}"
    port = listener.getsockname()[1]
    before = snapshot(console)
    with ThreadPoolExecutor(1) as pool:
        result = pool.submit(receive_frame, listener, args.peer, target, width * height * 3 // 2)
        checked(console, f"{test} {width} {height} /tmp/{name}.jpg --export "
                f"--send {args.host} {port}" + (" --nv21" if nv21 else ""), 20)
        result.result(timeout=5)
    after = snapshot(console)
    if after[0] - before[0] != 1 or after[2] != before[2]:
        raise RuntimeError(f"{name} {fmt}: unexpected IRQ delta or retained DMA-BUFs")
    quality = compare(args.directory, name, width, height, nv21)
    console.record(f"[host] {name} {fmt.upper()} "
                   f"PSNR Y/U/V {'/'.join(f'{p:.3f}' for p, _ in quality)} dB "
                   f"MAE Y/U/V {'/'.join(f'{m:.4f}' for _, m in quality)}\n".encode())
    if any(psnr < 38 or mae > 2 for psnr, mae in quality):
        raise RuntimeError(f"{name} {fmt}: software reference mismatch")


def acceptance(console, args):
    checked(console, "uname -r; cat /proc/cmdline; cat /proc/mounts; "
            "mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true")
    state = checked(console, "uname -r; cat /proc/mounts; "
                    "ls -l /dev/video0 /dev/video1 /dev/media0 /dev/dri/card0 /dev/snd/pcmC0D0p && "
                    "cat /sys/class/video4linux/video0/name /sys/class/video4linux/video1/name && "
                    "! ls -d /sys/kernel/debug/hi3798_codec_probe >/dev/null 2>&1 && "
                    "! ls -d /sys/block/mmcblk0 >/dev/null 2>&1 && "
                    "! ls -d /sys/block/sda >/dev/null 2>&1")
    if (RELEASE not in state or b"rootfs / rootfs " not in state or
            b"hi3798mv320-jpgd" not in state or re.search(rb"(?m)^/dev/\S+ ", state)):
        raise RuntimeError("Unexpected RAM root or device registration")
    names = ("jpgd-test", *(f"{name}.jpg" for name, _, _ in SAMPLES),
             "1080p-sequence.bin", "malformed.jpg")
    stage(console, args, args.directory, names)
    stage(console, args, args.audio, AUDIO)
    checked(console, "chmod 755 /tmp/jpgd-test /tmp/ld-musl-aarch64.so.1 /tmp/vdp-player")
    test = "/tmp/jpgd-test /dev/video1"
    media_smoke(console)
    start = snapshot(console)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind((args.host, args.yuv_port))
        listener.listen(1)
        listener.settimeout(8)
        for sample in SAMPLES:
            single_frame(console, args, listener, test, sample, False)
        single_frame(console, args, listener, test, SAMPLES[1], True)

    before = snapshot(console)
    checked(console, f"{test} 1280 720 /tmp/malformed.jpg --expect-error")
    if snapshot(console)[0] != before[0]:
        raise RuntimeError("Invalid JPEG generated a JPGD interrupt")

    before = snapshot(console)
    run = checked(console, f"{test} 1920 1080 --packed /tmp/1080p-sequence.bin {FRAMES} --export", 30)
    after = snapshot(console)
    if (f"frames={FRAMES} rejected=0 ".encode() not in run or after[0] - before[0] != FRAMES
            or after[2] != before[2] or after[1] < before[1] - 512):
        raise RuntimeError("Continuous decode, IRQ or memory recovery mismatch")
    console.record(f"[host] {FRAMES} frames, SPI 97 +{after[0]-before[0]}, "
                   f"CmaFree {before[1]} -> {after[1]} KiB, DMA-BUF {before[2]} -> {after[2]}\n".encode())
    media_smoke(console)
    finish = snapshot(console)
    if finish[2] != start[2]:
        raise RuntimeError("DMA-BUFs remained after final regression")
    crg = checked(console, "r=/sys/kernel/debug/regmap/dummy-syscon@0x00000000f8a22000/registers; "
                  "dd if=$r bs=14 skip=31 count=1 2>/dev/null && "
                  "dd if=$r bs=14 skip=205 count=1 2>/dev/null")
    if b"07c: 00000010" not in crg or b"334: 00000010" not in crg:
        raise RuntimeError("JPGD core and SMMU did not return to reset with gates closed")
    dmesg = checked(console, "dmesg", 30)
    if FAULT.search(dmesg):
        raise RuntimeError("Kernel reported a fault")
    console.record(b"[host] JPGD acceptance passed with SMMU INTID 181=0\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--prepare", action="store_true")
    mode.add_argument("--running-ram", action="store_true")
    parser.add_argument("--bl31", type=Path, default=Path(os.environ.get("TEMP", ".")) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--audio", type=Path, default=ROOT / "artifacts/arm64-7.3.6-aiao/serve")
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8088)
    parser.add_argument("--yuv-port", type=int, default=8089)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare(args.directory)
        return

    log = args.log or (args.directory / "board.txt")
    port = serial.Serial(baudrate=115200, bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
                         stopbits=serial.STOPBITS_ONE, timeout=.10, write_timeout=4)
    port.port = args.port
    port.dtr = port.rts = False
    with log.open("xb") as file, port:
        console = LogConsole(port, file)
        if args.running_ram:
            port.write(b"\r")
            console.expect(SHELL_PROMPT, 6)
        else:
            ram_boot(console, args)
        acceptance(console, args)


if __name__ == "__main__":
    main()
