"""Boot the HG680KD encoder over COM4 and verify both H.264 decoders."""

import argparse
from binascii import crc32
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import gzip
from hashlib import file_digest, sha256
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

import numpy as np
import serial

from jpgd_ram_test import LogConsole, checked, ram_boot, stage
from media_regression_ram import BUSYBOX, IRQS, irq_delta, mesa_regression, regressions
from serial_boot import SHELL_PROMPT
from vdec_request_fixture import START_CODE, fixture
from vdp_ram_test import count_irq


RELEASE = b"7.3.6-hg680kd-arm64-venc-ippp"
NODES = (
    "/dev/video0", "/dev/video1", "/dev/video2", "/dev/media0",
    "/dev/dri/card0", "/dev/dri/renderD128", "/dev/snd/pcmC0D0p",
)
CMA = "/sys/kernel/debug/cma/reserved/used"
CRG = "/sys/kernel/debug/clk/venc/crg_registers"
FATAL = re.compile(
    rb"SError|external abort|Kernel panic|SMMU (?:fault|error)\b|SMMU faults=[1-9]|"
    rb"BUG:|WARNING:|disabling IRQ|VENC CRG restore failed|bus drain failed|"
    rb"unsafe VEDU|isolating DMA|invalid VENC DMA|encode failed", re.I,
)


@dataclass(frozen=True)
class Case:
    width: int
    height: int
    fmt: str
    qp: int

    @property
    def name(self):
        return f"{self.height}p-{self.fmt.lower()}-qp{self.qp}"


CASES = (
    Case(1280, 720, "NV12", 24),
    Case(1280, 720, "NV21", 32),
    Case(1920, 1080, "NV12", 32),
    Case(1920, 1080, "NV21", 24),
)


class PacedSerial(serial.Serial):
    def write(self, data):
        for off in range(0, len(data), 36):
            super().write(data[off:off + 36])
            time.sleep(0.015)
        return len(data)


def prepare(directory, case, frames):
    raw = directory / f"{case.name}.raw"
    packed = raw.with_suffix(".raw.gz")
    size = case.width * case.height * 3 // 2 * frames
    if raw.exists() and raw.stat().st_size == size and packed.exists():
        return raw, packed
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size={case.width}x{case.height}:rate=30",
        "-frames:v", str(frames), "-pix_fmt", case.fmt.lower(), "-threads", "1",
        "-f", "rawvideo", str(raw),
    ], check=True)
    with raw.open("rb") as src, gzip.open(packed, "wb", compresslevel=1) as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    return raw, packed


def verify_boot(console, release=RELEASE):
    out = checked(console, "uname -r; cat /proc/mounts /proc/partitions; "
                  "ls -d /dev/sd* /dev/mmcblk* /dev/mtd* 2>/dev/null || :")
    if not re.search(rb"(?m)^" + re.escape(release) + rb"\r?$", out) or b"rootfs / rootfs " not in out:
        raise RuntimeError("wrong RAM kernel or root filesystem")
    if re.search(rb"(?m)^/dev/\S+|^\s+\d+\s+\d+\s+\d+\s+\w+\r?$", out):
        raise RuntimeError("a persistent block device is visible")
    checked(console, "dmesg -n 4; ls -d /sys/kernel/debug/clk >/dev/null 2>&1 || "
            "mount -t debugfs debugfs /sys/kernel/debug")
    for path in NODES:
        if not re.search(rb"(?m)^c[rw-]{9} ", checked(console, f"ls -l {path}")):
            raise RuntimeError(f"{path} is not a character device")
    names = checked(console, "cat /sys/class/video4linux/video0/name "
                    "/sys/class/video4linux/video1/name /sys/class/video4linux/video2/name")
    if (b"hi3798mv320-vdec" not in names.lower() or b"hi3798mv320-jpgd" not in names.lower() or
            b"hi3798mv320-venc" not in names.lower()):
        raise RuntimeError(f"incorrect video node binding: {names!r}")
    checked(console, "if ls /sys/kernel/debug/hi3798_venc_probe >/dev/null 2>&1; then exit 1; fi")
    log = checked(console, "dmesg", 40)
    if FATAL.search(log) or b"registered as /dev/video2" not in log:
        raise RuntimeError("boot log contains a fault or is missing VENC registration")


def snapshot(console):
    state = checked(console, f"{IRQS}; grep '^Cma' /proc/meminfo; "
                    f"printf 'CMA_USED='; cat {CMA}; cat {CRG}; "
                    "cat /sys/kernel/debug/dma_buf/bufinfo")
    irq, smmu = count_irq(state, 133), count_irq(state, 181)
    free = re.search(rb"(?m)^CmaFree:\s+(\d+)", state)
    used = re.search(rb"(?m)^CMA_USED=(\d+)", state)
    dma = re.search(rb"(?m)^Total\s+(\d+) objects,\s+(\d+) bytes", state)
    crg = re.search(rb"(?m)^0x08c=(0x[\da-f]+) 0x32c=(0x[\da-f]+)", state)
    if len(irq) != 1 or smmu != [0] or not all((free, used, dma, crg)):
        raise RuntimeError(f"missing resource counters or nonzero SMMU IRQ: {state!r}")
    return {
        "irq": irq[0], "smmu_irq": smmu[0], "cma_free_kib": int(free[1]),
        "cma_used_pages": int(used[1]), "dma_buf": list(map(int, dma.groups())),
        "crg": [value.decode() for value in crg.groups()],
    }


def get_stream(console, args, local):
    remote = f"/tmp/{local.name}"
    state = checked(console, f"/tmp/busybox.static stat -c 'SIZE=%s' {remote} && sha256sum {remote}")
    size_match = re.search(rb"(?m)^SIZE=(\d+)", state)
    digest = re.search(rb"(?m)^([a-f0-9]{64})  /tmp/", state)
    if not (size_match and digest):
        raise RuntimeError(f"missing file size or SHA256 for {remote}")
    size, expected = int(size_match[1]), digest[1].decode()

    def receive(listener):
        link, client = listener.accept()
        with link, local.open("wb") as output:
            if client[0] != args.peer:
                raise RuntimeError(f"unexpected TCP upload client {client}")
            link.settimeout(25)
            remaining = size
            while remaining:
                data = link.recv(min(remaining, 1 << 16))
                if not data:
                    raise IOError("H.264 upload ended early")
                output.write(data)
                remaining -= len(data)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener, ThreadPoolExecutor(1) as pool:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.upload_port))
        listener.listen(1)
        listener.settimeout(25)
        transfer = pool.submit(receive, listener)
        checked(console, f"/tmp/busybox.static nc -w 20 {args.host} {args.upload_port} <{remote}", 50)
        transfer.result(timeout=5)
    with local.open("rb") as source:
        if file_digest(source, "sha256").hexdigest() != expected:
            raise IOError("H.264 upload SHA256 mismatch")
    return size, expected


def metric(raw, decoded, case, frames, coded_height):
    w, h = case.width, case.height
    coded_y = w * coded_height
    original = np.memmap(raw, dtype=np.uint8, mode="r", shape=(frames, w * h * 3 // 2))
    result = np.memmap(decoded, dtype=np.uint8, mode="r", shape=(frames, coded_y * 3 // 2))
    vu = case.fmt == "NV21"
    squared = np.zeros(3, dtype=np.float64)
    peak_mse = np.zeros(3, dtype=np.float64)
    samples = w * h * np.array([1.0, .25, .25])

    def error(a, b):
        delta = a.astype(np.int32) - b
        return (delta * delta).sum(dtype=np.float64)

    for a, b in zip(original, result):
        ac, bc = a[w * h:], b[coded_y:coded_y + w * h // 2]
        errors = np.array([
            error(a[:w * h], b[:w * h]),
            error(ac[int(vu)::2], bc[1::2]),
            error(ac[int(not vu)::2], bc[0::2]),
        ])
        squared += errors
        peak_mse = np.maximum(peak_mse, errors / samples)
    return tuple((10 * np.log10(255 ** 2 / mse)).tolist()
                 for mse in (squared / (frames * samples), peak_mse))


def verify_stream(case, stream, stem, positions, gop, p_qp):
    sps, pps, slices = fixture(stream, stem, all_idr=gop == 1, ippp=gop > 1)
    data = stream.read_bytes()
    types = [data[code.end()] & 31 for code in START_CODE.finditer(data)]
    expected_types = [kind for f in positions for kind in ([7, 8, 5] if f == 0 else [1])]
    if (len(slices) != len(positions) or types != expected_types or
            sps["profile_idc"] != 66 or not sps["constraint_set1_flag"] or
            sps["max_num_ref_frames"] != 1 or sps["pic_order_cnt_type"] != (2 if gop == 1 else 0) or
            pps["entropy_coding_mode_flag"]):
        raise RuntimeError("bitstream does not match single-slice Constrained Baseline IPPP")
    idr_pic_id = 0
    for i, (f, slc) in enumerate(zip(positions, slices)):
        qp = p_qp if f else case.qp
        if (slc["frame_num"] != f or not slc["nal_ref_idc"] or
                slc["slice_type"] % 5 != (0 if f else 2) or
                slc["slice_qp_delta"] + pps["pic_init_qp_minus26"] + 26 != qp or
                (gop > 1 and slc["pic_order_cnt_lsb"] != 2 * f) or
                (f and (slc["ref_pic_list_modification_flag_l0"] or
                        slc["adaptive_ref_pic_marking_mode_flag"])) or
                (not f and slc["idr_pic_id"] != idr_pic_id)):
            raise RuntimeError(f"invalid slice header at frame {i}: {slc}")
        if not f:
            idr_pic_id ^= 1

    probe = subprocess.run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
        "-show_entries", "frame=key_frame,pict_type:stream=profile,width,height", "-of", "json",
        str(stream),
    ], check=True, capture_output=True, text=True)
    info = json.loads(probe.stdout)
    if (probe.stderr or info["streams"] != [
            {"profile": "Constrained Baseline", "width": case.width, "height": case.height}
    ] or [(p["key_frame"], p["pict_type"]) for p in info["frames"]] != [
            (0, "P") if f else (1, "I") for f in positions
    ]):
        raise RuntimeError(f"ffprobe disagrees with expected frame structure: {probe.stderr}")
    return sps


def vdec_loopback(console, args, case, stream, raw, positions, gop, p_qp):
    stem = stream.with_name(stream.stem + "-request")
    sps = verify_stream(case, stream, stem, positions, gop, p_qp)
    lines = [line.split() for line in stem.with_suffix(".txt").read_text().splitlines()]
    sequence, rows = lines[0], lines[1:]
    w, coded_h, n = map(int, sequence[:3])
    width = w - 2 * (sps.get("frame_crop_left_offset", 0) + sps.get("frame_crop_right_offset", 0))
    height = coded_h - 2 * (sps.get("frame_crop_top_offset", 0) + sps.get("frame_crop_bottom_offset", 0))
    if (width, height, n) != (case.width, case.height, len(positions)):
        raise RuntimeError("SPS dimensions, crop or frame count mismatch")
    reference = stem.with_suffix(".nv21")
    psnr, min_psnr = metric(raw, reference, case, n, coded_h)
    if min(psnr) < 32 or min(min_psnr) < 30:
        raise RuntimeError(f"unexpected PSNR or chroma order: {psnr}, minima {min_psnr}")

    # Each IPPP decode batch starts at an IDR and includes its references.
    starts = list(range(0, n, 12)) if gop == 1 else [i for i, f in enumerate(positions) if not f]
    ends = starts[1:] + [n]
    frame_bytes = w * coded_h * 3 // 2
    names, hashes = [], []
    with stem.with_suffix(".264").open("rb") as slices_file, reference.open("rb") as sw:
        for i, end in zip(starts, ends):
            batch = rows[i:end]
            seq = sequence.copy()
            seq[2] = str(len(batch))
            name = f"{stem.name}-{i:03d}"
            manifest, data = name + ".txt", name + ".264"
            (args.directory / manifest).write_text(
                "".join(" ".join(row) + "\n" for row in [seq, *batch]), encoding="ascii", newline="\n",
            )
            (args.directory / data).write_bytes(slices_file.read(sum(int(row[0]) for row in batch)))
            hashes.append(sha256(sw.read(frame_bytes * len(batch))).hexdigest())
            names.extend((manifest, data))
    stage(console, args, args.directory, names)
    before = checked(console, IRQS)
    out = checked(
        console,
        f'cd /tmp && for f in {stem.name}-*.txt; do '
        'vdec-request /dev/video0 /dev/media0 "$f" "${f%.txt}.264" venc-vdec.nv21 '
        '>venc-vdec.log || { cat venc-vdec.log; exit 1; }; '
        'sha256sum venc-vdec.nv21; done',
        120,
    )
    after = checked(console, IRQS)
    delta = irq_delta(before, after, 169)
    actual = [m.decode() for m in re.findall(rb"(?m)^([a-f\d]{64})  venc-vdec.nv21", out)]
    if actual != hashes or delta != n:
        raise RuntimeError(f"VDEC SHA256 or IRQ mismatch for {case.name}, IRQ={delta}")
    checked(console, f"/tmp/busybox.static rm -f /tmp/{stem.name}-*.txt /tmp/{stem.name}-*.264 "
            "/tmp/venc-vdec.nv21 /tmp/venc-vdec.log")
    return psnr, min_psnr, hashes, delta


def run_case(console, args, case, raw, gop):
    before = snapshot(console)
    p_qp = case.qp if args.p_qp is None else args.p_qp
    force = "" if args.force_key_at is None else f"--force-key-at {args.force_key_at}"
    stream = args.directory / f"{case.name}-gop{gop}.h264"
    out = checked(console, f"cd /tmp && ./venc-test --gop {gop} --i-qp {case.qp} --p-qp {p_qp} "
                  f"{force} /dev/video2 {case.width} {case.height} {case.fmt} "
                  f"{args.frames} {raw.name} {stream.name}", 100)
    perf = re.search(rb"(?m)^VENC .* bytes=(\d+) fps=([.\d]+) avg_ms=([.\d]+) "
                     rb"max_ms=([.\d]+) irq=(\d+)", out)
    cpu = re.findall(rb"(?m)^CPU[0-3] ([.\d]+)%", out)
    rate = re.search(rb"(?m)^RATE input_fps=(\d+) kbps=([.\d]+) Mbps=([.\d]+) i_frames=(\d+) "
                     rb"p_frames=(\d+) i_bytes_avg=([.\d]+) p_bytes_avg=([.\d]+)", out)
    positions = [(i if args.force_key_at is None or i < args.force_key_at
                  else i - args.force_key_at) % gop for i in range(args.frames)]
    if (not perf or int(perf[5]) != args.frames or len(cpu) != 4 or not rate or
            (int(rate[4]), int(rate[5])) != (positions.count(0), args.frames - positions.count(0))):
        raise RuntimeError(f"missing performance measurements for {case.name}")
    size, digest = get_stream(console, args, stream)
    if size != int(perf[1]):
        raise RuntimeError("sum of bytesused does not match the stream file")
    checked(console, f"/tmp/busybox.static rm /tmp/{stream.name}")
    after = snapshot(console)
    stats = checked(console, "dmesg | grep 'SMMU faults='")
    drained, faults, quarantine = map(int, re.findall(
        rb"drained=(\d+) SMMU faults=(\d+) quarantined=(\d+)", stats,
    )[-1])
    if (after["irq"] - before["irq"] != args.frames or drained != after["irq"] or
            faults or quarantine or before["cma_used_pages"] != after["cma_used_pages"] or
            before["crg"] != after["crg"] or after["dma_buf"] != [0, 0]):
        raise RuntimeError(f"VENC drain, resource or interrupt mismatch: {before} -> {after}")
    psnr, min_psnr, hashes, vdec_irq = vdec_loopback(console, args, case, stream, raw, positions, gop, p_qp)
    result = {
        "case": stream.stem, "gop": gop, "i_qp": case.qp, "p_qp": p_qp,
        "force_key_at": args.force_key_at, "source_rate_fps": int(rate[1]),
        "frames": args.frames, "bytes": size, "sha256": digest,
        "kbps": float(rate[2]), "mbps": float(rate[3]), "i_frames": int(rate[4]), "p_frames": int(rate[5]),
        "i_bytes_avg": float(rate[6]), "p_bytes_avg": float(rate[7]),
        "fps": float(perf[2]), "avg_ms": float(perf[3]), "max_ms": float(perf[4]),
        "cpu": list(map(float, cpu)), "psnr_yuv": psnr, "min_frame_psnr_yuv": min_psnr,
        "encode_irq_delta": args.frames, "decode_irq_delta": vdec_irq,
        "before": before, "after_streamoff": after, "drained_total": drained,
        "venc_smmu_faults": faults, "vdec_nv21_batch_sha256": hashes,
    }
    console.record(f"[host] {stream.stem} PASS, Y/U/V PSNR {psnr}, VDEC +{vdec_irq}\n".encode())
    return result


def finish(console, report):
    report["final"] = snapshot(console)
    log = checked(console, "dmesg", 50)
    fault = FATAL.search(log)
    if fault:
        raise RuntimeError(f"kernel error: {fault[0]!r}")
    # VENC's CMA allocations must be released after each stream, before the
    # other media drivers are exercised. Record their final usage separately.
    if (report["final"]["irq"] != report["after_encode"]["irq"] or
            report["final"]["crg"] != report["baseline"]["crg"] or report["final"]["dma_buf"] != [0, 0]):
        raise RuntimeError("unexpected VENC IRQ, CRG or DMA-BUF state after media regression")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", type=Path)
    ap.add_argument("--log", type=Path)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--gops", type=int, nargs="+", default=[1, 30])
    ap.add_argument("--p-qp", type=int)
    ap.add_argument("--force-key-at", type=int)
    ap.add_argument("--cases", nargs="+", choices=[case.name for case in CASES])
    ap.add_argument("--host", default="192.168.6.172")
    ap.add_argument("--peer", default="192.168.6.249")
    ap.add_argument("--http-port", type=int, default=8088)
    ap.add_argument("--upload-port", type=int, default=8089)
    ap.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) / "hg680kd-el2-stock-atf-256k.bin")
    ap.add_argument("--running-ram", action="store_true")
    ap.add_argument("--prepare-only", action="store_true")
    ap.add_argument("--skip-regression", action="store_true")
    args = ap.parse_args()
    if not 1 <= args.frames <= 120:
        ap.error("--frames must be between 1 and 120")
    if not all(1 <= gop <= 32 for gop in args.gops):
        ap.error("this VDEC fixture supports GOP lengths from 1 to 32")
    if args.p_qp is not None and not 0 <= args.p_qp <= 51:
        ap.error("--p-qp must be between 0 and 51")
    if args.force_key_at is not None and not 0 <= args.force_key_at < args.frames:
        ap.error("--force-key-at is a zero-based frame index")
    if not args.prepare_only and args.log is None:
        ap.error("--log is required for board testing")
    cases = [case for case in CASES if not args.cases or case.name in args.cases]
    args.directory.mkdir(parents=True, exist_ok=True)
    inputs = [prepare(args.directory, case, args.frames) for case in cases]
    if args.prepare_only:
        return

    image = args.directory / "boot.img"
    with image.open("rb") as f:
        digest = file_digest(f, "sha256").hexdigest()
    if digest != (args.directory / "SHA256SUMS").read_text().split()[0]:
        raise RuntimeError("boot image SHA256 mismatch")
    port = PacedSerial(baudrate=115200, bytesize=8, parity="N", stopbits=1, timeout=.1, write_timeout=4)
    port.port = "COM4"
    port.dtr = port.rts = False
    result_path = args.log.with_suffix(".json")
    report = {"boot_img_sha256": digest, "boot_img_crc32": f"{crc32(image.read_bytes()):08x}",
              "cases": []}
    with args.log.open("xb") as log, port:
        console = LogConsole(port, log)
        if args.running_ram:
            port.write(b"\r")
            console.expect(SHELL_PROMPT, 6)
        else:
            ram_boot(console, args)
        verify_boot(console, (args.directory / "kernel.release").read_bytes().strip())
        stage(console, args, BUSYBOX, ("busybox.static",))
        stage(console, args, args.directory, ("venc-test",))
        checked(console, "chmod 755 /tmp/busybox.static /tmp/venc-test")
        report["baseline"] = snapshot(console)
        for case, (raw, packed) in zip(cases, inputs):
            stage(console, args, args.directory, (packed.name,))
            checked(console, f"cd /tmp && ./busybox.static gzip -dc {packed.name} >{raw.name} && "
                    f"./busybox.static rm {packed.name}", 75)
            for gop in args.gops:
                report["cases"].append(run_case(console, args, case, raw, gop))
                result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
            checked(console, f"/tmp/busybox.static rm /tmp/{raw.name}")
        report["after_encode"] = snapshot(console)
        if report["after_encode"]["cma_used_pages"] != report["baseline"]["cma_used_pages"]:
            raise RuntimeError("CMA usage did not recover after the encode and decode cases")
        if not args.skip_regression:
            regressions(console, args)
            report["after_media"] = snapshot(console)
            report["mesa_irq_delta"] = mesa_regression(console, args)
            report["media_regression"] = "pass"
        finish(console, report)
        result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
