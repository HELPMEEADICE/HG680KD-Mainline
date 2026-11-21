"""Serve one named boot image to one U-Boot client, over read-only TFTP."""

import argparse
from pathlib import Path
import socket
import struct


def send_and_ack(transport, packet, block):
    ack = struct.pack(">HH", 4, block)
    for _ in range(6):
        transport.send(packet)
        try:
            if transport.recv(1500) == ack:
                return
        except TimeoutError:
            pass
    raise TimeoutError(f"client did not acknowledge TFTP block {block}")


def serve(listener, name, image, allowed_peer):
    request_prefix = b"\0\1" + name.encode("ascii") + b"\0octet\0"
    while True:
        request, peer = listener.recvfrom(4096)
        if peer[0] == allowed_peer and request.startswith(request_prefix):
            break
    fields = request[len(request_prefix):].split(b"\0")
    options = dict(zip(fields[::2], fields[1::2]))
    reply = []
    size = 512
    if b"blksize" in options:
        size = min(1468, int(options[b"blksize"]))
        if size < 8:
            raise ValueError("invalid TFTP block size")
        reply.extend((b"blksize", str(size).encode()))
    if b"tsize" in options:
        reply.extend((b"tsize", str(len(image)).encode()))

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as transport:
        transport.bind((listener.getsockname()[0], 0))
        transport.connect(peer)
        transport.settimeout(2)
        if reply:
            send_and_ack(transport, b"\0\6" + b"\0".join(reply) + b"\0", 0)
        for index, offset in enumerate(range(0, len(image) + 1, size), 1):
            block = index % 65536
            packet = struct.pack(">HH", 3, block) + image[offset:offset + size]
            send_and_ack(transport, packet, block)
    print(f"Sent {len(image)} bytes of {name} to {peer[0]}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--bind", required=True)
    parser.add_argument("--peer", required=True)
    parser.add_argument("--port", type=int, default=1069)
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind((args.bind, args.port))
        listener.settimeout(args.timeout)
        print(f"Listening on UDP {args.bind}:{args.port} for {args.peer}", flush=True)
        serve(listener, args.image.name, args.image.read_bytes(), args.peer)


if __name__ == "__main__":
    main()
