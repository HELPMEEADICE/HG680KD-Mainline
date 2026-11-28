"""Prepare V4L2 MPEG requests and FFmpeg NV21 references in decode order."""

import argparse
import json
from pathlib import Path
import re
from struct import pack
import subprocess
from tempfile import TemporaryDirectory


START = re.compile(rb"\x00\x00\x01(.)", re.S)
ZIGZAG = (
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
)
DEFAULT_INTRA = (
    8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
)


class Bits:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def get(self, width):
        if self.pos + width > len(self.data) * 8:
            raise ValueError("truncated MPEG header")
        result = 0
        for i in range(self.pos, self.pos + width):
            result = result * 2 + (self.data[i // 8] >> (7 - i % 8) & 1)
        self.pos += width
        return result


def sequence(bits):
    width, height = bits.get(12), bits.get(12)
    bits.get(26)
    if bits.get(1) != 1:
        raise ValueError("invalid MPEG sequence marker")
    vbv = bits.get(10)
    bits.get(1)
    intra = bytes(bits.get(8) for _ in range(64)) if bits.get(1) else bytes(
        DEFAULT_INTRA[i] for i in ZIGZAG
    )
    non_intra = bytes(bits.get(8) for _ in range(64)) if bits.get(1) else bytes([16] * 64)
    return width, height, vbv, [intra, non_intra, intra, non_intra]


def stateful_chunks(data):
    """Return the elementary stream packets, including its initial headers."""
    info = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_streams",
        "-show_frames", "-show_packets", "-show_entries",
        "stream=codec_name,width,height:packet=pos,size:frame=pkt_pos,pict_type",
        "-of", "json", str(data),
    ]))
    entries = info["packets_and_frames"]
    packets = [(int(x["pos"]), int(x["size"])) for x in entries if x["type"] == "packet"]
    order = {int(x["pkt_pos"]): i for i, x in enumerate(
        x for x in entries if x["type"] == "frame"
    )}
    if len(packets) != len(order) or any(pos not in order for pos, _ in packets):
        raise ValueError("expected one complete picture per FFprobe packet")
    return info["streams"][0], packets, order


def mpeg2_metadata(packet, state):
    start_codes = list(START.finditer(packet))
    for index, start in enumerate(start_codes):
        code = start[1][0]
        if 1 <= code <= 0xaf:
            break
        end = start_codes[index + 1].start() if index + 1 < len(start_codes) else len(packet)
        bits = Bits(packet[start.end():end])
        if code == 0xb3:
            state["width"], state["height"], state["vbv"], state["quant"] = sequence(bits)
        elif code == 0x00:
            bits.get(10)
            kind = bits.get(3)
        elif code == 0xb5:
            extension = bits.get(4)
            if extension == 1:
                state["profile"] = bits.get(8)
                state["progressive"] = bits.get(1)
                state["chroma"] = bits.get(2)
                state["width"] |= bits.get(2) << 12
                state["height"] |= bits.get(2) << 12
                bits.get(13)
                state["vbv"] |= bits.get(8) << 10
            elif extension == 3:
                for matrix in range(4):
                    if bits.get(1):
                        state["quant"][matrix] = bytes(bits.get(8) for _ in range(64))
            elif extension == 8:
                f_code = [bits.get(4) for _ in range(4)]
                precision, structure = bits.get(2), bits.get(2)
                flags = sum(bits.get(1) << i for i in range(7))
                bits.get(1)  # chroma_420_type
                flags |= bits.get(1) << 7
    else:
        raise ValueError("picture has no MPEG-2 slices")

    forward = backward = 0
    if kind == 2:
        forward = state["recent"]
    elif kind == 3:
        forward, backward = state["past"], state["recent"]
    if kind != 3:
        state["past"], state["recent"] = state.get("recent"), state["id"]
    seq = pack("<HHIHBB", *(state[key] for key in (
        "width", "height", "vbv", "profile", "chroma", "progressive"
    )))
    picture = pack("<QQI7B5x", backward * 1000, forward * 1000, flags,
                   *f_code, kind, structure, precision)
    return seq + picture + b"".join(state["quant"]), packet[start.start():]


def prepare(path, prefix):
    info, packets, order = stateful_chunks(path)
    codec = info["codec_name"]
    if codec not in ("mpeg1video", "mpeg2video"):
        raise ValueError(f"unsupported codec {codec}")
    width, height = info["width"], info["height"]
    size = width * height * 3 // 2
    stream = path.read_bytes()
    with prefix.with_suffix(".requests").open("wb") as output:
        output.write(pack("<4I", int(codec[4]), width, height, len(packets)))
        state = {}
        for i, (pos, count) in enumerate(packets, 1):
            packet = stream[pos:pos + count]
            if codec == "mpeg2video":
                state["id"] = i
                metadata, packet = mpeg2_metadata(packet, state)
            else:
                metadata = b""
            output.write(pack("<I", len(packet)) + metadata + packet)

    with TemporaryDirectory(dir=prefix.parent) as work:
        raw = Path(work) / "display.nv21"
        subprocess.run([
            "ffmpeg", "-v", "error", "-nostdin", "-xerror", "-y", "-i", str(path),
            "-pix_fmt", "nv21", "-f", "rawvideo", str(raw),
        ], check=True)
        if raw.stat().st_size != len(packets) * size:
            raise ValueError("unexpected software reference length")
        with raw.open("rb") as input_file, prefix.with_suffix(".ref.nv21").open("wb") as out:
            for pos, _ in packets:
                input_file.seek(order[pos] * size)
                out.write(input_file.read(size))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stream", type=Path)
    parser.add_argument("prefix", type=Path)
    args = parser.parse_args()
    prepare(args.stream, args.prefix)
