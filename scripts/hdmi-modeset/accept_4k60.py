"""Validate all three native HDMI modes on the storage-free HG680KD RAM image."""

import argparse
import os
from pathlib import Path
import re
import time

from board import (AUDIO, LogConsole, PROBE, PacedSerial, SCRIPTS, SNAPSHOT, checked,
                   count_irq, ram_boot, run_mode, shell, stage)
from locks_rx import parse_locks, rx_tags, save_json, source_status, verify_tags
from receiver import Receiver, native_snapshot

RELEASE = b"7.3.6-hg680kd-arm64-hdmi-4k60"
MODES = (
    ("1080p60-first", "1080p60", 60, 148500, 16, 0),
    ("2160p30", "2160p30", 30, 297000, 95, 0),
    ("2160p60-first", "2160p60", 60, 297000, 97, 3),
    ("1080p60-return", "1080p60", 60, 148500, 16, 0),
    ("2160p60-return", "2160p60", 60, 297000, 97, 3),
)
BLOCK_DEVICE = re.compile(rb"(?m)^/dev/\S+|^\s+\d+\s+\d+\s+\d+\s+\w+\r?$")


def fields(raw, names, base=10):
    fields_by_name = dict(re.findall(rb"(\w+)=([-\da-f]+)(?=\s|$)", raw))
    return {name: int(fields_by_name[name.encode()], base) for name in names.split()}


def reg(raw, name):
    return int(re.search(rb"(?m)^" + re.escape(name.encode()) + rb"=([0-9a-f]+)", raw)[1], 16)


def link_state(raw):
    state = fields(raw, "edid_max_tmds_khz vic_rgb vic_420 scdc_present scdc_version_error "
                        "configured verified")
    state.update(fields(raw, "version config_before config_after scrambler status", 16))
    if not state["configured"]:
        for name in ("config_before", "config_after", "scrambler", "status"):
            state[name] = None
    if state["scdc_version_error"]:
        state["version"] = None
    state["tx_encoder"] = reg(raw, "packet[260]")
    state["phy_clock_pattern"] = [reg(raw, "phy[050]") >> 9 & 0x3ff,
                                  reg(raw, "phy[054]") & 0x3fffffff]
    state["ddc"] = {index: reg(raw, f"ddc[{index}]")
                    for index in ("008", "010", "030", "034", "038")}
    return state


def verify_link(state, colorspace):
    return (state["edid_max_tmds_khz"] == 300000
            and state["vic_rgb"] == int(colorspace == 0)
            and state["vic_420"] == int(colorspace == 3)
            and state["scdc_present"] == 0
            and state["scdc_version_error"] == 0 and state["version"] == 0
            and state["configured"] == state["verified"] == 1
            and state["config_before"] & 3 == state["config_after"] & 3 == 0
            and state["scrambler"] & 1 == 0 and state["status"] & 1 == 1
            and state["tx_encoder"] & 0x27 == 0
            and state["phy_clock_pattern"] == [0x1f, 0x01f07c1f]
            and state["ddc"] == {"008": 0, "010": 0x20, "030": 0, "034": 0, "038": 0})


def verify_receiver(before, sample, nominal, vic, colorspace, result):
    return (sample["geometry_consistent"]
            and sample["pixel_active"] == result["active"]
            and sample["pixel_total"] == result["totals"]
            and sample["avi_checksum_ok"] and sample["avi"] == result["avi"]
            and sample["avi"] != before["avi"] and sample["vic"] == vic
            and sample["avi_colorspace"] == colorspace and sample["gcp_reported_bpc"] == 8
            and sample["rx_acr_n"] == 6144 and abs(sample["rx_acr_cts"] - result["sw_cts"]) <= 1
            and sample["acr_8bpc_frame_hz"] is not None
            and round(sample["acr_8bpc_frame_hz"], 3) == nominal)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--bl31", type=Path, default=Path(os.environ["TEMP"]) /
                        "hg680kd-el2-stock-atf-256k.bin")
    parser.add_argument("--host", default="192.168.6.172")
    parser.add_argument("--peer", default="192.168.6.249")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--capture-video", default="USB2 Video")
    parser.add_argument("--capture-audio", default="数字音频接口 (USB2 Digital Audio)")
    args = parser.parse_args()

    port = PacedSerial(port=None, baudrate=115200, bytesize=8, parity="N", stopbits=1,
                       timeout=.08, write_timeout=3)
    port.port = "COM4"
    port.dtr = port.rts = False
    results = {}
    with port, (args.directory / "4k60-com4.txt").open("wb") as log, Receiver() as rx:
        console = LogConsole(port, log)
        ram_boot(console, args)
        raw = checked(console, "uname -r; cat /proc/mounts /proc/partitions")
        if (not re.search(rb"(?m)^" + re.escape(RELEASE) + rb"\r?$", raw)
                or b"rootfs / rootfs " not in raw or BLOCK_DEVICE.search(raw)):
            raise RuntimeError("Expected the storage-free 4K60 RAM image")
        checked(console, "dmesg -n 4; ls /sys/kernel/debug/clk >/dev/null 2>&1 || "
                         "mount -t debugfs debugfs /sys/kernel/debug")
        inherited = checked(console, SNAPSHOT)
        (args.directory / "firmware-snapshot.txt").write_bytes(inherited)
        if count_irq(inherited, 181) != [0] or b"underflow=0 fetch=00000000" not in inherited:
            raise RuntimeError("Initial VDP/SMMU error counters are nonzero or missing")

        # The 300 MHz sink must reject RGB 4K60 before disturbing the active link.
        prior = native_snapshot(rx)
        rc, report = shell(console, f"echo 2160p60-rgb > {PROBE}; rc=$?; cat {PROBE}; (exit $rc)")
        (args.directory / "reject-2160p60-rgb.txt").write_bytes(report)
        rejected = native_snapshot(rx)
        if (not rc or b"mode=firmware step=inherited error=-95" not in report
                or prior["avi"] != rejected["avi"]):
            raise RuntimeError("The sink capability check failed to reject 594 MHz RGB")
        save_json(args.directory, "receiver-initial.json", rejected)

        stage(console, args, SCRIPTS.parent / "artifacts/arm64-7.3.6-usb-full", AUDIO)
        checked(console, "chmod 755 /tmp/vdp-player /tmp/ld-musl-aarch64.so.1")
        previous = rejected
        for label, mode, nominal, clock, vic, colorspace in MODES:
            result = run_mode(console, args, label, mode, nominal, clock, vic, colorspace)
            raw = (args.directory / f"{label}-switch.txt").read_bytes()
            result["link_at_switch"] = link_state(raw)
            result["locks"] = parse_locks(raw)
            stable = source_status(console, "scdc-read")
            (args.directory / f"{label}-scdc-stable.txt").write_bytes(stable)
            result["link"] = link_state(stable)
            marker, tags = rx_tags(console, rx, args.directory, label, vic)
            result["expected_max_cll"] = marker
            result["receiver"] = tags["on"]
            result["receiver_avi_before"] = previous["avi"]
            save_json(args.directory, f"{label}-rx-tags.json", tags)
            results[label] = result
            save_json(args.directory, "results.json", results)
            verify_tags(tags, marker)
            if not (result["board_pass"] and result["capture_pass"]
                    and result["intact_bar_frames"] == result["frames"]
                    and verify_link(result["link"], colorspace)
                    and verify_receiver(previous, tags["on"], nominal, vic, colorspace, result)):
                raise RuntimeError(f"{label} did not pass, see results.json and COM4 log")
            previous = tags["on"]
            print(f"{label}: RX {previous['acr_8bpc_frame_hz']:.3f} Hz, "
                  f"VT1 {result['vt1_hz']:.6f} Hz, SCDC config 0x00, "
                  f"{result['intact_bar_frames']}/{result['frames']} frames, zero XRUN", flush=True)

        raw = checked(console, f"dmesg; {SNAPSHOT}; cat /proc/mounts /proc/partitions")
        (args.directory / "final.txt").write_bytes(raw)
        if (count_irq(raw, 181) != [0] or b"underflow=0 fetch=00000000" not in raw
                or b"receiver_tags=0" not in raw or reg(raw, "packet[1b4]") & 3
                or BLOCK_DEVICE.search(raw)
                or re.search(rb"SError|external abort|Kernel panic|BUG:|WARNING:|disabling IRQ", raw)):
            raise RuntimeError("Final hardware, packets or storage check failed")


if __name__ == "__main__":
    main()
