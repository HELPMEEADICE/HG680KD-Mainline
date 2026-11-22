"""Transfer, verify and optionally boot a RAM image on the HG680KD."""

import argparse
import binascii
from pathlib import Path
import re
import sys
import time

import serial

PROMPT = b"fastboot#"
SHELL_PROMPT = b"\n# "
ACK = b"\x06"
NAK = b"\x15"
CAN = b"\x18"
EOT = b"\x04"


class Console:
    def __init__(self, port, log):
        self.port = port
        self.log = log

    def record(self, data):
        self.log.write(data)
        self.log.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

    def expect(self, pattern, timeout=10):
        deadline = time.monotonic() + timeout
        received = bytearray()
        boot_interrupted = False
        search = pattern.search if isinstance(pattern, re.Pattern) else None
        while time.monotonic() < deadline:
            data = self.port.read(1)
            if not data:
                continue
            received.extend(data)
            self.record(data)
            if search(received) if search else pattern in received:
                return bytes(received)
            if not boot_interrupted and received.endswith((b"Autobooting in", b"stop autoboot")):
                self.interrupt_autoboot()
                boot_interrupted = True
        raise TimeoutError(f"serial did not send {pattern!r}")

    def command(self, text, timeout=10):
        self.port.write(text.encode("ascii") + b"\r")
        output = self.expect(PROMPT, timeout)
        if b"Bootrom start" in output:
            raise RuntimeError(f"board reset during {text!r}; autoboot interrupted")
        return output

    def response(self, valid, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            char = self.port.read(1)
            if char and char in valid:
                return char
        return b""

    def transfer(self, image, address, baud):
        initial_baud = self.port.baudrate
        fast = baud != initial_baud
        self.port.write(f"loadx {address:x} {baud}\r".encode("ascii"))
        if fast:
            self.expect(f"Switch baudrate to {baud} bps and press ENTER ...".encode())
            time.sleep(0.15)
            self.port.baudrate = baud
            self.port.write(b"\r")
        sent = False
        try:
            self.expect(f"at {baud} bps...".encode())
            if self.response(b"C", timeout=10) != b"C":
                raise TimeoutError("U-Boot did not offer XMODEM CRC mode")
            start = time.monotonic()
            retries = 0
            for index, offset in enumerate(range(0, len(image), 1024), 1):
                payload = image[offset:offset + 1024].ljust(1024, b"\x1a")
                packet = bytes((2, index & 255, (255 - index) & 255))
                packet += payload + binascii.crc_hqx(payload, 0).to_bytes(2, "big")
                for _ in range(10):
                    self.port.write(packet)
                    reply = self.response(ACK + NAK + CAN)
                    if reply == ACK:
                        break
                    if reply == CAN:
                        raise IOError(f"U-Boot cancelled XMODEM at {offset:#x}")
                    retries += 1
                else:
                    raise IOError(f"no ACK for XMODEM block at {offset:#x}")
                if index % 256 == 0:
                    print(f"Sent {offset + 1024:,} / {len(image):,} bytes", flush=True)
            for _ in range(3):
                self.port.write(EOT)
                if self.response(ACK + NAK) == ACK:
                    sent = True
                    break
            if not sent:
                raise IOError("U-Boot did not acknowledge end of transfer")
            self.record(
                f"\n[host] XMODEM sent {len(image)} bytes in {time.monotonic() - start:.1f} s"
                f" with {retries} retries\n".encode()
            )
        finally:
            if not sent:
                self.port.write(CAN * 8)
            if fast:
                try:
                    self.expect(f"Switch baudrate to {initial_baud} bps and press ESC ...".encode())
                finally:
                    time.sleep(0.15)
                    self.port.baudrate = initial_baud
                    self.port.write(b"\x1b")
        self.expect(PROMPT)

    def verify(self, image, address):
        expected = binascii.crc32(image)
        output = self.command(f"crc32 {address:x} {len(image):x}")
        match = re.search(rb"==>\s+([0-9a-fA-F]{8})", output)
        if not match or int(match[1], 16) != expected:
            raise IOError(f"board CRC32 does not match local CRC32 {expected:08x}")
        self.record(f"\n[host] CRC32 matches local image: {expected:08x}\n".encode())

    def capture(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            data = self.port.read(self.port.in_waiting or 1)
            if data:
                self.record(data)

    def shell_command(self, text):
        start = time.monotonic()
        self.port.write(text.encode("ascii") + b"\r")
        self.expect(SHELL_PROMPT, timeout=30)
        self.record(f"\n[host] shell command elapsed: {time.monotonic() - start:.2f} s\n".encode())

    def interrupt_autoboot(self):
        for _ in range(5):
            self.port.write(b"\x03")
            time.sleep(0.03)

    def watch_uboot(self, seconds):
        deadline = time.monotonic() + seconds
        window = bytearray()
        while time.monotonic() < deadline:
            data = self.port.read(self.port.in_waiting or 1)
            if not data:
                continue
            self.record(data)
            window.extend(data)
            if PROMPT in window:
                return
            if b"Autobooting in" in window or b"stop autoboot" in window:
                self.interrupt_autoboot()
                window.clear()
            elif len(window) > 2048:
                del window[:-128]
        raise TimeoutError("U-Boot prompt did not appear")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--address", type=lambda value: int(value, 16), default=0x01FFBFC0)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--tftp", help="use tftpboot with this filename, and verify against --image")
    parser.add_argument("--before", action="append", default=[])
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--boot-command")
    parser.add_argument("--linux", action="store_true", help="connect to an existing BusyBox shell")
    parser.add_argument("--shell-command", action="append", default=[])
    parser.add_argument("--reboot-to-uboot", action="store_true")
    parser.add_argument("--watch-uboot", action="store_true", help="capture and interrupt a manual reboot")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    if args.tftp and not args.image:
        parser.error("--tftp needs the local --image for CRC verification")

    port = serial.Serial(baudrate=115200, timeout=0.08, write_timeout=3)
    port.port = args.port
    port.dtr = port.rts = False
    with port, args.log.open("wb") as log:
        console = Console(port, log)
        if args.watch_uboot:
            port.write(b"\x03")
            console.watch_uboot(args.seconds)
            return
        if args.linux:
            port.write(b"\r")
            console.expect(SHELL_PROMPT, timeout=6)
        else:
            port.write(b"\x03")
            console.expect(PROMPT, timeout=6)
            for command in args.before:
                console.command(command, timeout=20)
            if args.image:
                image = args.image.read_bytes()
                if args.tftp:
                    console.command(f"tftpboot {args.address:x} {args.tftp}", timeout=20)
                else:
                    console.transfer(image, args.address, args.baud)
                console.verify(image, args.address)
            for command in args.command:
                console.command(command, timeout=20)
            if args.boot_command:
                port.write(args.boot_command.encode("ascii") + b"\r")
                if args.shell_command:
                    console.expect(SHELL_PROMPT, timeout=args.seconds)
                else:
                    console.capture(args.seconds)
        for command in args.shell_command:
            console.shell_command(command)
        if args.reboot_to_uboot:
            port.write(b"reboot -f\r")
            console.watch_uboot(90)


if __name__ == "__main__":
    main()
