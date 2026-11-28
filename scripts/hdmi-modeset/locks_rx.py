"""Verify CRG/PHY lock discrimination and capture bare HDMI RX registers."""

import argparse
import json
import os
from pathlib import Path
import re
import sys
import time

from receiver import Receiver, native_snapshot

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
from jpgd_ram_test import LogConsole, checked, ram_boot
from vdp_ram_test import count_irq
from venc_ram_test import PacedSerial

PROBE = "/sys/kernel/debug/hi3798_hdmi_modeset"
IRQS = "grep -E 'GICv3 +(122|181) ' /proc/interrupts"
MODES = (
    ("1080p60-first", "1080p60", [1920, 1080], [2200, 1125], 16, 60),
    ("2160p30", "2160p30", [3840, 2160], [4400, 2250], 95, 30),
    ("1080p60-return", "1080p60", [1920, 1080], [2200, 1125], 16, 60),
)


def source_status(console, operation=None):
    prefix = f"echo {operation} > {PROBE} && " if operation else ""
    return checked(console, f"{prefix}cat {PROBE} && {IRQS}", 15)


def parse_locks(raw):
    bitmap = re.search(
        rb"hpll_on=([0-9a-f]+) hpll_off=([0-9a-f]+) hpll_restored=([0-9a-f]+) "
        rb"hpll_mask=([0-9a-f]+) relock_ns=(\d+)", raw)
    before, off, restored, mask = [int(x, 16) for x in bitmap.groups()[:4]]
    samples = re.search(rb"locks_before=([0-9a-f/]+) phy_off=([0-9a-f/]+) "
                        rb"active=([0-9a-f/]+) phy_unlock=(\d)", raw)
    phy = [[int(x, 16) for x in row.split(b"/")] for row in samples.groups()[:3]]
    if (before, off, restored, mask) != (0x7f, 0x77, 0x7f, 8):
        raise RuntimeError("CRG HPLL lock bitmap did not pass the isolated power cycle")
    if ([row[0] for row in phy] != [0x7f] * 3 or
            [bool(row[2] & 0x100000) for row in phy] != [True, False, True] or
            samples[4] != b"1"):
        raise RuntimeError("PHY clock lock did not leave and regain lock with HPLL steady")
    return {"hpll": [before, off, restored], "hpll_mask": mask, "relock_ns": int(bitmap[5]),
            "phy_power_sequence": phy}


def verify_native(sample, active, totals, vic, hz):
    if (not sample["geometry_consistent"] or sample["active"] != active or sample["total"] != totals
            or not sample["avi_checksum_ok"] or sample["vic"] != vic or sample["avi_colorspace"] != 0
            or sample["acr_rgb8_frame_hz"] is None or abs(sample["acr_rgb8_frame_hz"] - hz) > .005):
        raise RuntimeError(f"HDMI receiver native timing mismatch for VIC {vic}: {sample}")


def save_json(dest, name, data):
    (dest / name).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def rx_tags(console, rx, out, label, vic):
    states = {}
    try:
        (out / f"{label}-tags-off-source.txt").write_bytes(
            source_status(console, "rx-tags-off"))
        time.sleep(.5)
        states["off"] = native_snapshot(rx)
        marker = 100 + vic
        command = "rx-tags-on"
        if states["off"]["drm_max_cll"] == marker:
            marker += 100
            command = "rx-tags-alt"
        (out / f"{label}-tags-on-source.txt").write_bytes(
            source_status(console, command))
        time.sleep(.5)
        states["on"] = native_snapshot(rx)
    finally:
        restored = source_status(console, "rx-tags-off")
        (out / f"{label}-tags-restored-source.txt").write_bytes(restored)
    time.sleep(.5)
    states["after_off"] = native_snapshot(rx)
    return marker, states


def verify_tags(states, marker):
    off, on = states["off"], states["on"]
    if (off["drm_max_cll"] == marker or
            on["gcp_depth_code"] != 4 or on["gcp_reported_bpc"] != 8 or
            not on["drm_checksum_ok"] or on["drm_eotf"] != 0 or
            on["drm_descriptor"] != 0 or on["drm_max_cll"] != marker):
        raise RuntimeError("HDMI RX did not capture a fresh SDR DRM packet and 24-bit GCP")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--kernel-release", default="7.3.6-hg680kd-arm64-hdmi-modeset-locks")
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--running-ram", action="store_true")
    parser.add_argument("--tags", action="store_true")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    port = PacedSerial(port=None, baudrate=115200, bytesize=8, parity="N", stopbits=1,
                       timeout=.08, write_timeout=3)
    port.port = "COM4"
    port.dtr = port.rts = False
    results = {}
    with port, (args.directory / "locks-rx-com4.txt").open("wb") as log:
        console = LogConsole(port, log)
        if not args.running_ram:
            ram_boot(console, args)
        raw = checked(console, "uname -r; cat /proc/mounts /proc/partitions")
        if (not re.search(rb"(?m)^" + re.escape(args.kernel_release.encode()) + rb"\r?$", raw)
                or b"rootfs / rootfs " not in raw
                or re.search(rb"(?m)^/dev/\S+|^\s+\d+\s+\d+\s+\d+\s+\w+\r?$", raw)):
            raise RuntimeError("Expected the storage-free modeset RAM image")
        checked(console, "dmesg -n 4; ls /sys/kernel/debug/clk >/dev/null 2>&1 || "
                         "mount -t debugfs debugfs /sys/kernel/debug")
        with Receiver() as rx:
            save_json(args.directory, "receiver-initial.json", native_snapshot(rx))
            for label, mode, active, total, vic, hz in MODES:
                # If reusing a running 1080p image, force a real transition first.
                if args.running_ram and label == MODES[0][0]:
                    source_status(console, "2160p30")
                raw = source_status(console, mode)
                (args.directory / f"{label}-locks.txt").write_bytes(raw)
                locks = parse_locks(raw)
                time.sleep(1.5)
                sample = native_snapshot(rx)
                verify_native(sample, active, total, vic, hz)
                if b"underflow=0 fetch=00000000" not in raw or count_irq(raw, 181) != [0]:
                    raise RuntimeError(f"VDP/SMMU check failed at {mode}")
                result = {"locks": locks, "receiver": sample}
                if args.tags:
                    marker, states = rx_tags(console, rx, args.directory, label, vic)
                    result["expected_max_cll"] = marker
                    result["tags"] = states
                    save_json(args.directory, f"{label}-rx-tags.json", states)
                    verify_native(states["on"], active, total, vic, hz)
                    verify_tags(states, marker)
                results[label] = result
                save_json(args.directory, "locks-rx.json", results)
                print(f"{label}: CRG bit 3, PHY +0x98 bit 20, VIC {vic}, native {active}/{total}",
                      flush=True)
        final = checked(console, f"cat {PROBE}; {IRQS}; cat /proc/mounts /proc/partitions", 15)
        (args.directory / "locks-rx-final.txt").write_bytes(final)
        if (b"underflow=0 fetch=00000000" not in final or count_irq(final, 181) != [0] or
                (args.tags and (b"receiver_tags=0" not in final or b"packet[1b4]=00000000" not in final)) or
                re.search(rb"(?m)^/dev/\S+|^\s+\d+\s+\d+\s+\d+\s+\w+\r?$", final)):
            raise RuntimeError("Final hardware/partitions check failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
