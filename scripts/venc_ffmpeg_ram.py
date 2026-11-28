"""Validate unmodified FFmpeg's stateful H.264 encoder on the HG680KD in RAM."""

import argparse
from binascii import crc32
from fractions import Fraction
from hashlib import file_digest, sha256
import json
import os
from pathlib import Path
import re
import subprocess

from jpgd_ram_test import LogConsole, checked, ram_boot, stage
from media_regression_ram import BUSYBOX, ROOT, regressions
from venc_ram_test import (CASES, Case, PacedSerial, finish, get_stream, prepare,
                           snapshot, vdec_loopback, verify_boot)
from vdec_ffmpeg_ram import shell


FRAMES = 120
GOP = 30
RATE = 30
INPUTS = ROOT / "artifacts/arm64-7.3.6-venc-ippp"


def encode_command(case, raw, output, profile):
    return (f"./ffmpeg -nostdin -y -v debug -xerror -nostats -benchmark "
            f"-f rawvideo -pixel_format nv12 -video_size {case.width}x{case.height} "
            f"-framerate {RATE} -i {raw.name} -an -pix_fmt nv12 "
            f"-c:v h264_v4l2m2m -g {GOP}{' -profile:v 578' if profile else ''} {output}")


def cpu_usage(output):
    blocks = re.findall(rb"(?ms)^--- (START|END) ---\r?\n(.*?)^--- DONE ---", output)
    if [name for name, _ in blocks] != [b"START", b"END"]:
        raise RuntimeError("missing FFmpeg timing counters")
    stats = [re.findall(rb"(?m)^cpu[0-3][ \t]+([ \d]+)\r?$", data) for _, data in blocks]
    uptimes = [float(re.search(rb"(?m)^([\d.]+) [\d.]+\r?$", data)[1]) for _, data in blocks]
    if any(len(block) != 4 for block in stats):
        raise RuntimeError("missing four-core CPU counters")
    usage = []
    for before, after in zip(*stats):
        delta = [int(b) - int(a) for a, b in zip(before.split(), after.split())][:8]
        usage.append(round(100 * (sum(delta) - delta[3] - delta[4]) / sum(delta), 2))
    return round(uptimes[1] - uptimes[0], 3), usage


def host_tool(*command):
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    if result.stderr:
        raise RuntimeError(f"{command[0]} wrote to stderr: {result.stderr}")
    return result.stdout


def verify_file(path, case):
    probe = json.loads(host_tool(
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_streams",
        "-show_frames", "-show_packets", "-of", "json", str(path),
    ))
    stream, = probe["streams"]
    entries = probe["packets_and_frames"]
    frames = [frame for frame in entries if frame["type"] == "frame"]
    packets = [packet for packet in entries if packet["type"] == "packet"]
    if (stream["codec_name"], stream["width"], stream["height"], stream["profile"], stream["level"]) != (
            "h264", case.width, case.height, "Constrained Baseline", 42):
        raise RuntimeError(f"unexpected stream format for {path.name}")
    if len(packets) != FRAMES or [(f["key_frame"], f["pict_type"]) for f in frames] != [
            (1, "I") if i % GOP == 0 else (0, "P") for i in range(FRAMES)]:
        raise RuntimeError(f"unexpected packet count or GOP in {path.name}")

    time_base = Fraction(stream["time_base"])
    for i, (packet, frame) in enumerate(zip(packets, frames)):
        if int(packet["duration"]) * time_base != Fraction(1, RATE):
            raise RuntimeError(f"incorrect coded frame interval at packet {i} in {path.name}")
        if path.suffix == ".mp4":
            if any(Fraction(entry[key]) * time_base != Fraction(i, RATE) for entry, key in (
                    (packet, "pts"), (packet, "dts"), (frame, "best_effort_timestamp"))):
                raise RuntimeError(f"non-continuous MP4 timestamps at frame {i}")
        elif "pts" in packet or "dts" in packet:
            raise RuntimeError("unexpected container timestamps in raw Annex B")

    hashes = host_tool("ffmpeg", "-v", "error", "-xerror", "-err_detect", "explode", "-i",
                       str(path), "-fps_mode", "passthrough", "-f", "framemd5", "-")
    frame_time_base = Fraction(re.search(r"(?m)^#tb 0: (\d+/\d+)$", hashes)[1])
    rows = [line.split(",") for line in hashes.splitlines() if not line.startswith("#")]
    decoded = [row[-1].strip() for row in rows]
    if len(decoded) != FRAMES or len(set(decoded)) != FRAMES:
        raise RuntimeError(f"strict software decode failed or motion was lost in {path.name}")
    for i, row in enumerate(rows):
        dts, pts, duration = (int(value) * frame_time_base for value in row[1:4])
        if (dts, pts, duration) != (Fraction(i, RATE), Fraction(i, RATE), Fraction(1, RATE)):
            raise RuntimeError(f"non-continuous decoded frame timing at frame {i} in {path.name}")
    path.with_suffix(path.suffix + ".ffprobe.json").write_text(json.dumps(probe, indent=2) + "\n")
    path.with_suffix(path.suffix + ".framemd5").write_text(hashes)
    return decoded, sum(int(packet["size"]) for packet in packets)


def run_encode(console, args, case, raw, suffix):
    name = f"{case.height}p-ffmpeg{suffix}"
    command = encode_command(case, raw, name, args.profile)
    trace = f"{name}.ioctl"
    run = (f"./strace -f -yy -o {trace} -e trace=ioctl " if args.trace else "") + command
    before = snapshot(console)
    status, output = shell(console, "cd /tmp && { "
                           "echo '--- START ---'; cat /proc/uptime; grep '^cpu[0-3] ' /proc/stat; "
                           "echo '--- DONE ---'; "
                           f"./busybox.static timeout -s KILL 45 {run} >{name}.debug 2>&1; rc=$?; "
                           "echo '--- END ---'; cat /proc/uptime; grep '^cpu[0-3] ' /proc/stat; "
                           f"echo '--- DONE ---'; cat {name}.debug; exit \"$rc\"; }}", 65)
    after = snapshot(console)
    if args.trace:
        get_stream(console, args, args.directory / trace)
    console.record(f"[host] {name} FFmpeg exit={status}, IRQ +{after['irq'] - before['irq']}\n".encode())
    result = {"output": name, "command": command, "exit": status, "before": before, "after": after}
    if status or b"Using device /dev/video2" not in output:
        if not args.probe:
            raise RuntimeError(f"FFmpeg failed or did not select /dev/video2 for {name}")
        return result, None

    path = args.directory / name
    size, digest = get_stream(console, args, path)
    elapsed, cpu = cpu_usage(output)
    result.update(bytes=size, sha256=digest, elapsed_seconds=elapsed, cpu_percent=cpu)
    if args.probe:
        return result, None

    stats = checked(console, "/tmp/venc-ioctl-smoke --log-status && dmesg | grep 'SMMU faults='")
    drained, faults, quarantined = map(int, re.findall(
        rb"drained=(\d+) SMMU faults=(\d+) quarantined=(\d+)", stats)[-1])
    if (drained != after["irq"] or after["irq"] - before["irq"] != FRAMES or faults or quarantined or
            before["cma_used_pages"] != after["cma_used_pages"] or before["crg"] != after["crg"] or
            after["dma_buf"] != [0, 0]):
        raise RuntimeError(f"VENC drain, resource or interrupt mismatch for {name}")
    hashes, payload = verify_file(path, case)
    annexb = path
    if suffix == ".mp4":
        annexb = path.with_name(path.stem + "-mp4.h264")
        host_tool("ffmpeg", "-y", "-v", "error", "-xerror", "-i", str(path), "-c:v", "copy",
                  "-bsf:v", "h264_mp4toannexb", "-f", "h264", str(annexb))
    positions = [n % GOP for n in range(FRAMES)]
    psnr, min_psnr, vdec_hashes, vdec_irq = vdec_loopback(console, args, case, annexb, raw, positions, GOP, case.qp)
    result.update(frames=FRAMES, fps=round(FRAMES / elapsed, 3),
                  payload_mbps=round(payload * 8 * RATE / FRAMES / 1e6, 6),
                  file_mbps=round(size * 8 * RATE / FRAMES / 1e6, 6),
                  encode_irq_delta=FRAMES, drained_total=drained, venc_smmu_faults=faults,
                  vdec_irq_delta=vdec_irq, vdec_batch_sha256=vdec_hashes,
                  psnr_yuv=psnr, min_frame_psnr_yuv=min_psnr)
    checked(console, f"/tmp/busybox.static rm -f /tmp/{name} /tmp/{name}.debug /tmp/{trace}")
    return result, hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="results and staged ffmpeg executable")
    parser.add_argument("--image-dir", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--probe", action="store_true", help="record baseline behavior without pass assertions")
    parser.add_argument("--trace", action="store_true", help="collect ioctl traces instead of benchmark timing")
    parser.add_argument("--profile", action="store_true", help="explicitly set H.264 Constrained Baseline")
    parser.add_argument("--skip-regression", action="store_true")
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8088)
    parser.add_argument("--upload-port", type=int, default=8089)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) / "hg680kd-el2-stock-atf-256k.bin")
    args = parser.parse_args()

    cases = [Case(case.width, case.height, case.fmt, 28) for case in CASES if case.fmt == "NV12"]
    inputs = [prepare(INPUTS, case, FRAMES) for case in CASES if case.fmt == "NV12"]
    results = args.directory
    image = (args.image_dir / "boot.img").read_bytes()
    digest = sha256(image).hexdigest()
    if digest != (args.image_dir / "SHA256SUMS").read_text().split()[0]:
        raise RuntimeError("boot image SHA256 mismatch")
    args.directory = args.image_dir
    port = PacedSerial(baudrate=115200, bytesize=8, parity="N", stopbits=1, timeout=.1, write_timeout=4)
    port.port = "COM4"
    port.dtr = port.rts = False
    result_path = args.log.with_suffix(".json")
    with (results / "ffmpeg").open("rb") as ffmpeg:
        ffmpeg_digest = file_digest(ffmpeg, "sha256").hexdigest()
    report = {"boot_img_sha256": digest, "boot_img_crc32": f"{crc32(image):08x}",
              "ffmpeg_sha256": ffmpeg_digest, "traced": args.trace, "cases": []}
    with args.log.open("xb") as log, port:
        console = LogConsole(port, log)
        ram_boot(console, args)
        verify_boot(console, (args.image_dir / "kernel.release").read_bytes().strip())
        args.directory = results
        stage(console, args, BUSYBOX, ("busybox.static",))
        tools = ["ffmpeg"]
        if args.trace:
            tools.append("strace")
        if not args.probe:
            tools.append("venc-ioctl-smoke")
        stage(console, args, results, tools)
        checked(console, "cd /tmp && chmod 755 busybox.static " + " ".join(tools))
        checked(console, "/tmp/ffmpeg -version; /tmp/ffmpeg -encoders | grep h264_v4l2m2m")
        report["baseline"] = snapshot(console)
        for case, (raw, packed) in zip(cases, inputs):
            stage(console, args, INPUTS, (packed.name,))
            checked(console, f"cd /tmp && ./busybox.static gzip -dc {packed.name} >{raw.name} && "
                    f"./busybox.static rm {packed.name}", 75)
            encodes = []
            for suffix in (".h264", ".mp4"):
                result, hashes = run_encode(console, args, case, raw, suffix)
                report["cases"].append(result)
                encodes.append(hashes)
                result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
            if not args.probe and encodes[0] != encodes[1]:
                raise RuntimeError(f"raw and MP4 decoded frames differ at {case.height}p")
            checked(console, f"/tmp/busybox.static rm /tmp/{raw.name}")
        report["after_encode"] = snapshot(console)
        if not args.probe:
            if report["after_encode"]["cma_used_pages"] != report["baseline"]["cma_used_pages"]:
                raise RuntimeError("CMA usage did not recover after encode and loopback")
            checked(console, "/tmp/venc-ioctl-smoke")
            report["ioctl_smoke"] = "pass"
            if not args.skip_regression:
                regressions(console, args)
                report["media_regression"] = "pass"
            finish(console, report)
        result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
