"""Validate MPEG hardware decode and media regressions on the HG680KD RAM image."""

import argparse
from binascii import crc32
from hashlib import file_digest
import json
import math
import os
from pathlib import Path
import re
from struct import unpack
import subprocess

import numpy as np

from jpgd_ram_test import LogConsole, checked, ram_boot, stage
from media_regression_ram import BUSYBOX, IRQS, irq_delta, mesa_regression, regressions
from vdec_ffmpeg_ram import shell
from vdp_ram_test import count_irq
from venc_ram_test import FATAL, PacedSerial, get_stream, verify_boot


ROOT = Path(__file__).resolve().parents[1]
RELEASE = b"7.3.6-hg680kd-arm64-vdec-ext"
VENC = ROOT / "artifacts/arm64-7.3.6-venc-ffmpeg"
VDP_CLIPS = ROOT / "artifacts/arm64-7.3.5-vdp/phase3/serve"
PLAYER = "./ld-musl-aarch64.so.1 --library-path . ./vdp-player"
DECODER = "/tmp/vdec-mpeg-test"
NODE = "/dev/video0 /dev/media0"


def digest(path):
    with path.open("rb") as f:
        return file_digest(f, "sha256").hexdigest()


def resources(console):
    out = checked(console, f"{IRQS}; grep '^Cma' /proc/meminfo; "
                  "printf 'CMA_USED='; cat /sys/kernel/debug/cma/reserved/used; "
                  "cat /sys/kernel/debug/dma_buf/bufinfo")
    free = re.search(rb"(?m)^CmaFree:\s+(\d+)", out)
    used = re.search(rb"(?m)^CMA_USED=(\d+)", out)
    dma = re.search(rb"(?m)^Total\s+(\d+) objects,\s+(\d+) bytes", out)
    irq, fault = count_irq(out, 169), count_irq(out, 181)
    if not all((free, used, dma)) or len(irq) != 1 or fault != [0]:
        raise RuntimeError("resource counters missing or VDEC SMMU INTID 181 is nonzero")
    return {"cma_free_kib": int(free[1]), "cma_used_pages": int(used[1]),
            "dma_buf": list(map(int, dma.groups())), "vdh_irq": irq[0], "smmu_irq": fault[0]}


def require_recovery(before, after, expected_irq):
    if (before["dma_buf"] != [0, 0] or after["dma_buf"] != [0, 0] or
            before["cma_used_pages"] != after["cma_used_pages"] or
            before["cma_free_kib"] - after["cma_free_kib"] > 4096 or
            after["vdh_irq"] - before["vdh_irq"] != expected_irq):
        raise RuntimeError(f"resource or IRQ mismatch {before} -> {after}")


def psnr(mse):
    return None if mse == 0 else 10 * math.log10(255 ** 2 / mse)


def compare(reference, decoded, width, height, count):
    y_size = width * height
    frame_size = y_size * 3 // 2
    shape = (count, frame_size)
    if any(f.stat().st_size != count * frame_size for f in (reference, decoded)):
        raise RuntimeError("NV21 output or reference length mismatch")
    expected = np.memmap(reference, dtype=np.uint8, mode="r", shape=shape)
    actual = np.memmap(decoded, dtype=np.uint8, mode="r", shape=shape)
    mse = np.empty((count, 3))
    for i, (a, b) in enumerate(zip(expected, actual)):
        delta = np.subtract(a, b, dtype=np.int32)
        squared = delta * delta
        mse[i] = [x.mean() for x in (squared[:y_size], squared[y_size + 1::2], squared[y_size::2])]
    worst = [psnr(x) for x in mse.max(axis=0)]
    if any(value is not None and value < 45 for value in worst):
        raise RuntimeError(f"unexpected image difference, minimum Y/U/V PSNR {worst}")
    return {"psnr_yuv_db": [psnr(x) for x in mse.mean(axis=0)], "min_psnr_yuv_db": worst,
            "frame_psnr_yuv_db": [[psnr(x) for x in row] for row in mse],
            "reference_sha256": digest(reference), "output_sha256": digest(decoded)}


def decode(console, args, name):
    fixture = args.directory / f"{name}.requests"
    with fixture.open("rb") as f:
        codec, width, height, frames = unpack("<4I", f.read(16))
    before = resources(console)
    command = f"{DECODER} {NODE} /tmp/{fixture.name}"
    out = checked(console, command, 35)
    perf = re.search(rb"(?m)^MPEG(\d) decoded=(\d+) fps=([.\d]+) ms=([.\d]+)", out)
    cpu = re.findall(rb"(?m)^CPU[0-3] ([.\d]+)%", out)
    if not perf or tuple(map(int, perf.groups()[:2])) != (codec, frames) or len(cpu) != 4:
        raise RuntimeError(f"missing decode measurements for {name}")
    after = resources(console)
    require_recovery(before, after, frames)

    output = args.directory / f"{name}.hw.nv21"
    checked(console, f"{command} /tmp/{output.name}", 45)
    get_stream(console, args, output, timeout=150)
    checked(console, f"/tmp/busybox.static rm -f /tmp/{output.name}")
    after_yuv = resources(console)
    require_recovery(after, after_yuv, frames)
    quality = compare(args.directory / f"{name}.ref.nv21", output, width, height, frames)
    console.record(f"[host] {name} {frames} frames, {float(perf[3]):.3f} FPS, "
                   f"Y/U/V PSNR {quality['psnr_yuv_db']}\n".encode())
    return {"width": width, "height": height, "frames": frames, "fps": float(perf[3]),
            "frame_ms": float(perf[4]), "cpu_busy_percent": list(map(float, cpu)),
            "vdh_irq_delta": frames, "before": before, "after_perf": after,
            "after_yuv": after_yuv, **quality}


def h264_regression(console, args, clip="h264-2160p60.mp4", frames=120, fps=60):
    directory = VDP_CLIPS if clip == "h264-1080p60.mp4" else args.directory
    stage(console, args, directory, (clip,))
    before = checked(console, IRQS)
    status, out = shell(console, f"cd /tmp && {PLAYER} {clip}", 40)
    after = checked(console, IRQS)
    play = re.search(rb"PLAY decoded=(\d+) displayed=(\d+) dropped=(\d+) .*?"
                     rb"missed_vblanks=(\d+) fps=([.\d]+)", out)
    vdh, vdp = (irq_delta(before, after, n) for n in (169, 122))
    result = {"status": status, "vdh_irq_delta": vdh, "vdp_irq_delta": vdp}
    if play:
        result.update(dict(zip(("decoded", "displayed", "dropped", "missed_vblanks"),
                               map(int, play.groups()[:4]))))
        result["fps"] = float(play[5])
    source = b"1920x1080" if directory == VDP_CLIPS else b"3840x2160"
    # The player counts every HDMI refresh without a flip between two frames.
    expected_gaps = (frames - 1) * (60 // fps - 1)
    result["pass"] = bool(not status and play and vdh == frames and vdp >= frames and
                          tuple(map(int, play.groups()[:4])) == (frames, frames, 0, expected_gaps) and
                          float(play[5]) >= fps - 0.1 and b"V0 source=" + source in out)
    return result


def venc_regression(console, args):
    stage(console, args, VENC, ("venc-test",))
    stage(console, args, args.directory, ("venc-regression.nv12",))
    checked(console, "chmod +x /tmp/venc-test")
    before = checked(console, IRQS)
    out = checked(console, "cd /tmp && ./venc-test --gop 12 --i-qp 32 --p-qp 32 "
                  "/dev/video2 1920 1080 NV12 12 venc-regression.nv12 venc-regression.h264", 25)
    after = checked(console, IRQS)
    delta = irq_delta(before, after, 133)
    if delta != 12 or not re.search(rb"(?m)^VENC .* irq=12\b", out):
        raise RuntimeError("VENC did not complete twelve hardware frames")
    local = args.directory / "venc-regression.h264"
    _, sha = get_stream(console, args, local)
    subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-xerror", "-i", str(local),
                    "-f", "null", "-"], check=True)
    return {"encoded": 12, "venc_irq_delta": delta, "sha256": sha}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8088)
    parser.add_argument("--upload-port", type=int, default=8089)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--running-ram", action="store_true")
    args = parser.parse_args()
    args.directory = args.directory.resolve()
    image = args.directory / "boot.img"
    sha = digest(image)
    if sha != (args.directory / "SHA256SUMS").read_text().split()[0]:
        raise RuntimeError("boot image SHA256 mismatch")
    report = {"boot_sha256": sha, "boot_crc32": f"{crc32(image.read_bytes()):08x}",
              "tested_running_ram": args.running_ram}
    names = ("mpeg1-1080p", "mpeg2-1080p")
    log_path = args.directory / "acceptance-com4.txt"
    result_path = args.directory / "acceptance.json"
    try:
        port = PacedSerial(baudrate=115200, bytesize=8, parity="N", stopbits=1,
                           timeout=.1, write_timeout=4)
        port.port = "COM4"
        port.dtr = port.rts = False
        with log_path.open("ab" if args.running_ram else "wb") as log, port:
            console = LogConsole(port, log)
            if not args.running_ram:
                ram_boot(console, args)
            verify_boot(console, RELEASE)
            stage(console, args, BUSYBOX, ("busybox.static",))
            stage(console, args, args.directory, ("vdec-mpeg-test",
                                                  *(f"{name}.requests" for name in names)))
            checked(console, f"chmod +x /tmp/busybox.static {DECODER}")
            for name in names:
                report[name] = decode(console, args, name)
            before = resources(console)
            out = checked(console, f"{DECODER} --tiled {NODE} /tmp/mpeg2-1080p.requests", 35)
            after = resources(console)
            require_recovery(before, after, 120)
            if b"Tiled CAPTURE DMA-BUF exported" not in out:
                raise RuntimeError("missing native DMA-BUF export")
            report["tiled_mpeg2"] = {"before": before, "after": after}
            report["venc"] = venc_regression(console, args)
            regressions(console, args)
            report["lima_jpgd_hevc_vdp_aiao"] = "pass"
            report["mesa_irq_delta"] = mesa_regression(console, args)
            report["h264_1080p60"] = h264_regression(console, args, "h264-1080p60.mp4", 1080, 60)
            report["h264_2160p30"] = h264_regression(console, args, "h264-2160p30-pad.mp4", 120, 30)
            report["h264_2160p60"] = h264_regression(console, args)
            report["final"] = resources(console)
            if report["final"]["dma_buf"] != [0, 0] or FATAL.search(checked(console, "dmesg", 55)):
                raise RuntimeError("media failure or live DMA-BUF after regression")
            if not report["h264_1080p60"]["pass"] or not report["h264_2160p30"]["pass"]:
                raise RuntimeError("H.264 regression at a supported resolution and rate")
    finally:
        result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
