"""Extract the VDH CABAC context table from the verified stock kernel."""

import argparse
import hashlib
from pathlib import Path


def checked_bytes(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"unexpected SHA256 for {path}")
    return data


def c_array(name, data):
    lines = [f"static const u8 {name}[] = {{"]
    for offset in range(0, len(data), 12):
        lines.append("\t" + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("factory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    kernel = checked_bytes(
        args.factory / "hg680kd-stock-kernel.bin",
        "a47592ad5a842035f0bd6e41fdce71cb89c2238aff57e2b333d60213af176724",
    )
    cabac_offset = 0xC176BF4C - 0xC0008000
    tables = c_array("h264_cabac_init", kernel[cabac_offset:cabac_offset + 0x1400])
    args.output.write_text(
        "/* SPDX-License-Identifier: GPL-2.0-only */\n"
        "/* VDH H.264 CABAC context initialization table. */\n"
        + tables + "\n",
        encoding="ascii",
        newline="\n",
    )


if __name__ == "__main__":
    main()
