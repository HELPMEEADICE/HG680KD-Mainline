"""Wrap an ARM64 Linux Image + DTB with a minimal AArch64 DTB-pointer shim as a legacy uImage (IH_ARCH_ARM64)."""

import argparse
from binascii import crc32
from hashlib import sha256
from pathlib import Path
import struct

LEGACY_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM64 = 22
IH_TYPE_KERNEL = 2
IH_COMP_NONE = 0

SHIM_ADDR = 0x01FE0000
DTB_ADDR = 0x01FE1000
IMAGE_ADDR = 0x02000000
UIMAGE_ADDR = SHIM_ADDR - 0x40  # 0x01FDFFC0 (XIP: Load == Entry == 0x01FE0000)
TIMESTAMP = 1764547200


def build_shim(dtb_addr: int, image_addr: int) -> bytes:
    # AArch64 entry shim: sets x0 = dtb_addr, x1 = x2 = x3 = 0, branches to image_addr
    insns = struct.pack(
        "<6I2Q",
        0x580000C0,  # ldr x0, #0x18 (dtb_addr)
        0xAA1F03E1,  # mov x1, xzr
        0xAA1F03E2,  # mov x2, xzr
        0xAA1F03E3,  # mov x3, xzr
        0x58000084,  # ldr x4, #0x10 (image_addr)
        0xD61F0080,  # br  x4
        dtb_addr,
        image_addr,
    )
    return insns.ljust(DTB_ADDR - SHIM_ADDR, b"\x00")


def pack_uimage(image: bytes, dtb: bytes) -> bytes:
    if len(image) < 64 or image[0x38:0x3C] != b"ARM\x64":
        raise ValueError("missing ARM64 Image magic ('ARM\\x64' at offset 0x38)")
    if len(dtb) < 40 or struct.unpack_from(">II", dtb) != (0xD00DFEED, len(dtb)):
        raise ValueError("invalid DTB magic or length")
    max_dtb = IMAGE_ADDR - DTB_ADDR
    if len(dtb) > max_dtb:
        raise ValueError(f"DTB ({len(dtb)} bytes) exceeds window ({max_dtb} bytes)")

    shim = build_shim(DTB_ADDR, IMAGE_ADDR)
    payload = shim + dtb.ljust(max_dtb, b"\x00") + image
    header = bytearray(struct.pack(
        ">7I4B32s",
        LEGACY_MAGIC,
        0,
        TIMESTAMP,
        len(payload),
        SHIM_ADDR,
        SHIM_ADDR,
        crc32(payload),
        IH_OS_LINUX,
        IH_ARCH_ARM64,
        IH_TYPE_KERNEL,
        IH_COMP_NONE,
        b"Linux HG680KD ARM64",
    ))
    struct.pack_into(">I", header, 4, crc32(header))
    return bytes(header) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--dtb", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    image = args.image.read_bytes()
    dtb = args.dtb.read_bytes()
    uimage = pack_uimage(image, dtb)
    args.out.mkdir(parents=True, exist_ok=True)
    out_file = args.out / "uImage"
    out_file.write_bytes(uimage)
    digest = sha256(uimage).hexdigest()
    (args.out / "BOOT-SHA256SUMS").write_text(f"{digest}  uImage\n", encoding="ascii")
    print(f"{digest}  uImage  ({len(uimage)} bytes, CRC32={crc32(uimage):08x}, load_at=0x{UIMAGE_ADDR:08x})")


if __name__ == "__main__":
    main()
