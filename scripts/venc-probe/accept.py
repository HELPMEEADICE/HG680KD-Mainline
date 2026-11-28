"""Run the HG680KD VENC RAM probe and compare both H.264 decoders."""

import argparse
import base64
from hashlib import sha256
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import serial

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from jpgd_ram_test import LogConsole, checked, ram_boot, stage
from media_regression_ram import BUSYBOX, IRQS, mesa_regression, regressions
from serial_boot import SHELL_PROMPT
from vdec_ffmpeg_ram import shell
from vdec_request_fixture import START_CODE, fixture, parse_trace
from vdp_ram_test import count_irq

WIDTH, HEIGHT = 320, 192
PIXELS = WIDTH * HEIGHT
RELEASE = b"7.3.6-hg680kd-arm64-venc-probe"
DEBUGFS = "/sys/kernel/debug/hi3798_venc_probe"


class PacedSerial(serial.Serial):
    def write(self, data):
        for start in range(0, len(data), 36):
            super().write(data[start:start + 36])
            time.sleep(0.015)
        return len(data)


def color_bars():
    colors = [(205, 128, 128), (186, 32, 142), (158, 165, 32), (137, 55, 50),
              (106, 201, 207), (72, 99, 212), (48, 214, 115), (25, 128, 128)]
    luma = bytes(y + row // 12 for row in range(HEIGHT)
                 for y, _, _ in colors for _ in range(WIDTH // len(colors)))
    uv = bytes(chroma for _ in range(HEIGHT // 2) for _, u, v in colors
               for _ in range(WIDTH // len(colors) // 2) for chroma in (u, v))
    return luma + uv


def receive(console, remote, local):
    out = checked(console, f"printf '\\nB64_BEGIN\\n'; /tmp/busybox.static base64 {remote} "
                  "&& printf '\\nB64_END\\n'", 65)
    match = re.search(rb"(?m)^B64_BEGIN\r?\n([A-Za-z0-9+/=\r\n]*)^B64_END\r?$", out)
    if not match:
        raise RuntimeError(f"Missing serial payload for {remote}")
    data = base64.b64decode(b"".join(match[1].split()), validate=True)
    local.write_bytes(data)
    return data


def nal_types(stream):
    markers = list(START_CODE.finditer(stream))
    if not markers or markers[0].start() != 0 or any(m.end() == len(stream) for m in markers):
        raise ValueError("Expected Annex-B start code at the programmed DMA address")
    return [stream[m.end()] & 31 for m in markers]


def assemble(raw, headers):
    types = nal_types(raw)
    if types.count(5) != 1 or any(1 <= nal < 5 for nal in types):
        raise RuntimeError(f"Expected one IDR Slice, found NAL types {types}")
    if 7 in types and 8 in types:
        return raw, types, 0
    if 7 in types or 8 in types or nal_types(headers) != [7, 8]:
        raise RuntimeError(f"Unexpected DMA parameter set combination {types}")
    return headers + raw, types, len(headers)


def psnr(src, decoded):
    error = sum((a - b) ** 2 for a, b in zip(src, decoded))
    return "inf" if not error else round(10 * math.log10(255**2 * len(src) / error), 4)


def check_macroblock_qp(path):
    log = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostdin", "-v", "debug", "-debug", "qp",
         "-threads", "1", "-i", str(path), "-f", "null", "-"],
        capture_output=True, check=True).stderr
    path.with_name("ffmpeg-qp.txt").write_bytes(log)
    rows = re.findall(rb"(?m)^\[h264 @ [^\]]+\]\s+(\d+)\s+(\d{40})\r?$", log)
    expected = [(str(row).encode(), b"28" * (WIDTH // 16)) for row in range(0, HEIGHT, 16)]
    if rows[-len(expected):] != expected:
        raise RuntimeError("Decoded macroblock QP grid does not match fixed QP 28")
    return WIDTH * HEIGHT // 256


def validate(console, args, source, raw, headers, window, output):
    directory = args.directory
    stream, types, prepend = assemble(raw, headers)
    path = directory / "frame.h264"
    path.write_bytes(stream)
    if window[:64] != b"\xa5" * 64 or window[64:64 + len(raw)] != raw:
        raise RuntimeError("DMA prefix guard or stream export differs")
    lengths = re.findall(rb"slot0 padded=(\d+) padding=(\d+) bytesused=(\d+)", output)
    if not lengths:
        raise RuntimeError("Missing slot 0 length evidence")
    padded, padding, valid = map(int, lengths[-1])
    if padded - padding != valid or len(raw) != valid:
        raise RuntimeError("Register length differs from exported bytesused")
    if (window[64 + valid:64 + padded] != bytes(padding)
            or window[64 + padded:] != b"\xcd" * 64):
        raise RuntimeError("DMA tail padding or unwritten canary differs")

    sw = directory / "ffmpeg.nv21"
    decode = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostdin", "-y", "-v", "error", "-xerror",
         "-err_detect", "explode", "-f", "h264", "-i", str(path),
         "-pix_fmt", "nv21", "-f", "rawvideo", str(sw)],
        capture_output=True, check=False)
    (directory / "ffmpeg-errors.txt").write_bytes(decode.stderr)
    if decode.returncode or decode.stderr:
        raise RuntimeError(f"FFmpeg strict decode failed: {decode.stderr.decode(errors='replace')}")
    truncated = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostdin", "-v", "error", "-xerror",
         "-err_detect", "explode", "-f", "h264", "-i", "pipe:0",
         "-pix_fmt", "nv21", "-f", "rawvideo", "pipe:1"],
        input=stream[:-1], capture_output=True, check=False)
    (directory / "ffmpeg-truncated-errors.txt").write_bytes(truncated.stderr)
    if not truncated.returncode or truncated.stdout:
        raise RuntimeError("Dropping the last DMA payload byte still decoded a frame")
    meta = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-show_frames", "-of", "json", str(path)],
        capture_output=True, check=True).stdout
    (directory / "ffprobe.json").write_bytes(meta)
    stream_info = json.loads(meta)
    assert len(stream_info["frames"]) == len(stream_info["streams"]) == 1
    frame, codec = stream_info["frames"][0], stream_info["streams"][0]
    if (frame["width"], frame["height"], frame["key_frame"], frame["pict_type"]) != (320, 192, 1, "I"):
        raise RuntimeError("FFmpeg detected unexpected frame metadata")
    trace = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "info", "-i", str(path),
         "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"],
        capture_output=True, check=True, text=True, encoding="utf-8").stderr
    (directory / "h264-headers.txt").write_text(trace, encoding="utf-8")
    sps, pps, slices = parse_trace(trace)
    if (sps["profile_idc"] != 66 or pps["entropy_coding_mode_flag"] != 0 or
            len(slices) != 1 or slices[0]["nal_unit_type"] != 5 or
            pps["pic_init_qp_minus26"] + slices[0]["slice_qp_delta"] != 2):
        raise RuntimeError("H.264 header does not match the requested QP/profile")

    decoded = sw.read_bytes()
    if len(decoded) != len(source):
        raise RuntimeError("FFmpeg output is not a single 320x192 frame")
    quality = {
        "Y": psnr(source[:PIXELS], decoded[:PIXELS]),
        "U": psnr(source[PIXELS::2], decoded[PIXELS + 1::2]),
        "V": psnr(source[PIXELS + 1::2], decoded[PIXELS::2]),
    }
    summary = dict(profile=codec["profile"], width=WIDTH, height=HEIGHT, qp=28,
                   macroblocks_at_qp_28=check_macroblock_qp(path),
                   raw_nal_types=types, parameter_set_bytes_added=prepend,
                   dma_padded=padded, dma_padding=padding, dma_bytesused=valid,
                   h264_bytes=len(stream), truncated_final_byte_rejected=True, psnr_db=quality,
                   ffmpeg_nv21_sha256=sha256(decoded).hexdigest())
    summary_path = directory / "result.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    console.record(f"[host] FFmpeg {codec['profile']} QP 28, DMA NALs {types}, added "
                   f"{prepend} parameter bytes, PSNR Y/U/V {quality}\n".encode())

    # The driver uses stateless requests, so transfer the AU's VCL NAL plus
    # the SPS/PPS and Slice metadata parsed from the same complete frame.
    fixture(path, directory / "request")
    stage(console, args, directory, ("request.txt", "request.264"))
    before = checked(console, IRQS)
    result = checked(console, "cd /tmp && vdec-request /dev/video0 /dev/media0 "
                     "request.txt request.264 venc-decoded.nv21 && sha256sum venc-decoded.nv21", 30)
    after = checked(console, IRQS)
    if f"{summary['ffmpeg_nv21_sha256']}  venc-decoded.nv21".encode() not in result:
        raise RuntimeError("VDEC NV21 differs from FFmpeg NV21")
    if count_irq(before + after, 181) != [0, 0] or (
            count_irq(after, 169)[0] - count_irq(before, 169)[0] != 1):
        raise RuntimeError("Unexpected VDEC/SMMU IRQ counters")
    hardware = receive(console, "/tmp/venc-decoded.nv21", directory / "vdec.nv21")
    if hardware != decoded:
        raise RuntimeError("Exported hardware decoder output differs from FFmpeg")
    summary["vdec_nv21_sha256"] = summary["ffmpeg_nv21_sha256"]
    summary["vdec_nv21_identical_bytes"] = len(hardware)
    summary["vdec_irq_delta"] = 1
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    console.record(b"[host] /dev/video0 NV21 SHA256 matches FFmpeg byte-for-byte\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", type=Path)
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--mode", choices=("bypass", "identity"), default="bypass")
    ap.add_argument("--live-kernel-log", action="store_true")
    run = ap.add_mutually_exclusive_group()
    run.add_argument("--regression-only", action="store_true")
    run.add_argument("--mesa-only", action="store_true")
    boot = ap.add_mutually_exclusive_group()
    boot.add_argument("--running-ram", action="store_true")
    boot.add_argument("--leave-usb", action="store_true")
    ap.add_argument("--host", default="192.168.6.172")
    ap.add_argument("--peer", default="192.168.6.249")
    ap.add_argument("--http-port", type=int, default=8088)
    ap.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) / "hg680kd-el2-stock-atf-256k.bin")
    args = ap.parse_args()
    if not args.running_ram:
        want = (args.directory / "SHA256SUMS").read_text().split()[0]
        if sha256((args.directory / "boot.img").read_bytes()).hexdigest() != want:
            raise RuntimeError("Local boot.img failed SHA256")
    source = color_bars()
    (args.directory / "input.nv12").write_bytes(source)
    port = PacedSerial(baudrate=115200, bytesize=8, parity="N", stopbits=1, timeout=.1, write_timeout=4)
    port.port = "COM4"
    port.dtr = port.rts = False
    with args.log.open("xb") as log, port:
        console = LogConsole(port, log)
        if args.leave_usb:
            port.write(b"\r")
            console.expect(b"root@hg680kd:~# ", 7)
            port.write(b"systemctl reboot\r")
            console.watch_uboot(85)
        if args.running_ram:
            port.write(b"\r")
            console.expect(SHELL_PROMPT, 7)
        else:
            ram_boot(console, args)
        out = checked(console, "uname -r; cat /proc/mounts; cat /proc/partitions; cat /proc/cmdline; "
                      "ls -d /dev/sd* /dev/mmcblk* /dev/mtd* 2>/dev/null || true")
        if RELEASE not in out or b"rootfs / rootfs " not in out or re.search(rb"(?m)^/dev/\S+", out):
            raise RuntimeError("Expected the dedicated RAM kernel without mounted block devices")
        checked(console, "dmesg -n 4; "
                f"(test -e {DEBUGFS}/run || mount -t debugfs debugfs /sys/kernel/debug); ls -l {DEBUGFS}")
        if args.regression_only:
            regressions(console, args)
            return
        if args.mesa_only:
            mesa_regression(console, args)
            return
        stage(console, args, BUSYBOX, ("busybox.static",))
        checked(console, "chmod 755 /tmp/busybox.static")
        stage(console, args, args.directory, ("input.nv12",))
        if args.live_kernel_log:
            checked(console, "dmesg -n 8")
        before = checked(console, IRQS)
        status, _ = shell(console, f"dd if=/tmp/input.nv12 of={DEBUGFS}/input bs=92160 && "
                          f"echo {args.mode} >{DEBUGFS}/run; rc=$?; sleep 1; exit $rc", 12)
        out = checked(console, "dmesg | grep -Ei 'venc|f8c80000'")
        after = checked(console, IRQS)
        raw = receive(console, f"{DEBUGFS}/raw", args.directory / "dma-raw.h264")
        window = receive(console, f"{DEBUGFS}/window", args.directory / "dma-window.bin")
        headers = receive(console, f"{DEBUGFS}/headers", args.directory / "software-headers.h264")
        if status:
            raise RuntimeError(f"VENC trigger returned {status}\n{out.decode(errors='replace')}")
        if count_irq(before + after, 181) != [0, 0] or (
                count_irq(after, 133)[0] - count_irq(before, 133)[0] != 1):
            raise RuntimeError("Unexpected VENC/SMMU IRQ counters")
        validate(console, args, source, raw, headers, window, out)


if __name__ == "__main__":
    main()
