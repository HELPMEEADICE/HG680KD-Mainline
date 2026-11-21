"""Wrap an appended-DTB ARM zImage for the stock HG680KD bootm paths."""

import argparse
from binascii import crc32
from hashlib import sha1, sha256
from pathlib import Path
import struct

LEGACY_MAGIC = 0x27051956
KERNEL_ADDRESS = 0x02000000
ANDROID_ADDRESS = 0x01FFBFC0
ANDROID_PAGE = 0x4000
TIMESTAMP = 1764547200


def legacy_image(zimage, dtb):
    if len(zimage) < 48 or struct.unpack_from("<I", zimage, 0x24)[0] != 0x016F2818:
        raise ValueError("missing ARM zImage magic")
    if len(dtb) < 40 or struct.unpack_from(">II", dtb) != (0xD00DFEED, len(dtb)):
        raise ValueError("invalid DTB magic or length")
    payload = zimage + dtb
    header = bytearray(struct.pack(
        ">7I4B32s", LEGACY_MAGIC, 0, TIMESTAMP, len(payload), KERNEL_ADDRESS,
        KERNEL_ADDRESS, crc32(payload), 5, 2, 2, 0, b"Linux HG680KD ARM32",
    ))
    struct.pack_into(">I", header, 4, crc32(header))
    return bytes(header) + payload


def android_image(legacy, stock):
    if len(stock) < ANDROID_PAGE + 64 or stock[:8] != b"ANDROID!":
        raise ValueError("expected the HG680KD stock Android boot image")
    stock_length, _, ramdisk_length, _, second_length, _, _, page, version = (
        struct.unpack_from("<9I", stock, 8)
    )
    if (ramdisk_length, second_length, page, version) != (0, 0, ANDROID_PAGE, 1):
        raise ValueError("unexpected stock boot header layout")
    if struct.unpack_from(">I", stock, page)[0] != LEGACY_MAGIC:
        raise ValueError("stock Android kernel is missing its inner uImage header")
    if stock_length > 0x7800 * 512 - page or len(legacy) > 0x7800 * 512 - page:
        raise ValueError("kernel exceeds the stock bootcmd's RAM load window")
    if ANDROID_ADDRESS + page + 64 != KERNEL_ADDRESS:
        raise ValueError("unexpected physical address for Android kernel payload")

    header = bytearray(stock[:page])
    struct.pack_into("<I", header, 8, len(legacy))
    header[64:576] = bytes(512)
    header[608:1632] = bytes(1024)
    header[576:608] = sha1(legacy + struct.pack("<4I", len(legacy), 0, 0, 0)).digest().ljust(32, b"\0")
    return bytes(header) + legacy + bytes(-len(legacy) % page)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zimage", required=True, type=Path)
    parser.add_argument("--dtb", required=True, type=Path)
    parser.add_argument("--stock-boot", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    zimage = args.zimage.read_bytes()
    dtb = args.dtb.read_bytes()
    legacy = legacy_image(zimage, dtb)
    android = android_image(legacy, args.stock_boot.read_bytes())
    args.out.mkdir(parents=True, exist_ok=True)
    checksums = []
    for name, data in (("zImage-dtb", zimage + dtb), ("uImage", legacy), ("boot.img", android)):
        (args.out / name).write_bytes(data)
        checksum = f"{sha256(data).hexdigest()}  {name}"
        checksums.append(checksum)
        print(f"{checksum}  ({len(data)} bytes, CRC32={crc32(data):08x})")
    (args.out / "BOOT-SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
