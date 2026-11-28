"""Read HDMI receiver measurements from the MacroSilicon USB HID interface."""

import ctypes as ct
from ctypes import wintypes as wt
import re
import subprocess


class Receiver:
    def __enter__(self):
        listing = subprocess.check_output([
            "pnputil", "/enum-interfaces", "/enabled", "/class",
            "{4d1e55b2-f16f-11cf-88cb-001111000030}",
        ]).decode("ascii", errors="ignore")
        paths = re.findall(r"\\\\\?\\HID#VID_345F&PID_2131&MI_04#[^\s]+", listing, re.I)
        if len(paths) != 1:
            raise RuntimeError("Expected one 345f:2131 MacroSilicon HID interface")
        kernel = ct.WinDLL("kernel32", use_last_error=True)
        self.hid = ct.WinDLL("hid", use_last_error=True)
        self.close = kernel.CloseHandle
        self.close.argtypes = (wt.HANDLE,)
        kernel.CreateFileW.argtypes = (wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID,
                                       wt.DWORD, wt.DWORD, wt.HANDLE)
        kernel.CreateFileW.restype = wt.HANDLE
        # This HID device has no report ID. Windows supplies a leading zero.
        self.device = kernel.CreateFileW(paths[0], 0xc0000000, 3, None, 3, 0, None)
        if self.device == wt.HANDLE(-1).value:
            raise ct.WinError(ct.get_last_error())
        for request in (self.hid.HidD_SetFeature, self.hid.HidD_GetFeature):
            request.argtypes = (wt.HANDLE, wt.LPVOID, wt.ULONG)
            request.restype = ct.c_ubyte
        return self

    def __exit__(self, *exc):
        self.close(self.device)

    def read(self, address):
        """Issue the B5 XDATA read and check the echoed command and address."""
        report = (ct.c_ubyte * 9)(0, 0xb5, address >> 8, address & 0xff)
        if not self.hid.HidD_SetFeature(self.device, report, len(report)):
            raise ct.WinError(ct.get_last_error())
        reply = (ct.c_ubyte * 9)()
        if not self.hid.HidD_GetFeature(self.device, reply, len(reply)):
            raise ct.WinError(ct.get_last_error())
        if reply[1:4] != report[1:4]:
            raise RuntimeError(f"Unexpected HID read reply {bytes(reply).hex()}")
        return reply[4]


def receiver_snapshot():
    """Sample receiver pipeline status, the observed VIC byte and sync counters."""
    addresses = (0xf006, 0xf007, 0xf030, 0xf03e, 0xf0c0, 0xf0c1,
                 0xf620, 0xf621, 0xf622, 0xf623, 0xf624, 0xf625,
                 0xf626, 0xf627, 0xf628, 0xf629)
    with Receiver() as receiver:
        return {f"{address:04x}": receiver.read(address) for address in addresses}


def native_snapshot(hid=None):
    """Read MS2131 HDMI RX measurements and captured packets before UVC scaling.

    ACR-derived frequency uses the test stream's 48 kHz audio reference. The
    undecoded RX clock counter is kept separately as a raw reading.
    """
    if hid is None:
        with Receiver() as receiver:
            return native_snapshot(receiver)

    def window(start, length):
        return bytes(hid.read(start + i) for i in range(length))

    def le(data, offset, size=2):
        return int.from_bytes(data[offset:offset + size], "little")

    signal = window(0xe14e, 36)
    timing = window(0xe184, 18)
    acr = window(0xe390, 8)
    raw_avi = window(0xe3a0, 17)
    gcp = hid.read(0xe380)
    raw_drm = window(0xe4c0, 31)
    drm = b"\x87" + raw_drm[:2] + raw_drm[4:]
    drm_ok = drm[1:3] == b"\x01\x1a" and sum(drm) % 256 == 0
    depth_code = gcp >> 4
    reported_bpc = {4: 8, 5: 10, 6: 12, 7: 16}.get(depth_code)
    avi = b"\x82" + raw_avi[:3] + raw_avi[4:]
    htotal, width = le(signal, 0), le(signal, 2)
    height, vtotal = le(signal, 0x1a), le(signal, 0x22)
    hactive, hblank, hfront, hsync, vactive, vblank, vfront, vsync, vback = (
        le(timing, index) for index in range(0, 18, 2)
    )
    n, cts = le(acr, 4, 3) & 0xfffff, le(acr, 0, 3) & 0xfffff
    avi_ok = avi[1:3] == b"\x02\x0d" and sum(avi) % 256 == 0
    colorspace = (avi[4] >> 5) & 7 if avi_ok else None
    geometry_ok = (width == hactive and height == vactive and
                   htotal == width + hblank and vtotal == height + vblank and
                   vblank - vfront - vsync == vback)
    char_hz = (128 * 48000 * cts / n) if n and cts else None
    rgb_frame_hz = (char_hz / (htotal * vtotal)
                    if char_hz and geometry_ok and colorspace == 0 and depth_code in (0, 4) else None)
    return {
        "active": [width, height],
        "total": [htotal, vtotal],
        "front": [hfront, vfront],
        "sync": [hsync, vsync],
        "back": [hblank - hfront - hsync, vback],
        "geometry_consistent": geometry_ok,
        "avi": avi.hex(" "),
        "avi_checksum_ok": avi_ok,
        "vic": avi[7] if avi_ok else None,
        "avi_colorspace": colorspace,
        "raw_e380": gcp,
        "gcp_depth_code": depth_code,
        "gcp_reported_bpc": reported_bpc,
        "raw_e4c0": raw_drm.hex(" "),
        "drm_checksum_ok": drm_ok,
        "drm_infoframe": drm.hex(" ") if drm_ok else None,
        "drm_eotf": drm[4] & 7 if drm_ok else None,
        "drm_descriptor": drm[5] & 7 if drm_ok else None,
        "drm_max_cll": le(drm, 26) if drm_ok else None,
        "rx_clock_raw_e160": le(signal, 0x12, 3),
        "rx_acr_n": n,
        "rx_acr_cts": cts,
        "acr_reference_sample_rate": 48000,
        "acr_tmds_hz": char_hz,
        "acr_rgb8_frame_hz": rgb_frame_hz,
        "raw_e14e": signal.hex(" "),
        "raw_e184": timing.hex(" "),
        "raw_e390": acr.hex(" "),
        "raw_e3a0": raw_avi.hex(" "),
    }


if __name__ == "__main__":
    import argparse
    import json

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", action="store_true")
    args = parser.parse_args()
    print(json.dumps(native_snapshot() if args.native else receiver_snapshot(), indent=2))
