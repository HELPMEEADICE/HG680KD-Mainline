"""Run the existing HG680KD media regressions on a RAM-only kernel."""

from pathlib import Path
import re

from jpgd_ram_test import checked, stage
from vdp_ram_test import count_irq

ROOT = Path(__file__).resolve().parents[1]
IRQS = "grep -E 'GICv3 +(116|122|126|129|133|169|181) ' /proc/interrupts"
LIMA = ROOT / "artifacts/arm64-7.3.6-lima-dualpp"
DISPLAY = ROOT / "artifacts/arm64-7.3.6-usb-display"
BUSYBOX = LIMA / "mesa"


def irq_delta(before, after, number):
    old, new = count_irq(before, number), count_irq(after, number)
    if len(old) != 1 or len(new) != 1 or count_irq(before + after, 181) != [0, 0]:
        raise RuntimeError(f"Missing IRQ {number} or SMMU INTID 181 increased")
    return new[0] - old[0]


def regressions(console, args, player_dir=DISPLAY):
    stage(console, args, LIMA, ("lima-test",))
    stage(console, args, DISPLAY, ("jpgd-test", "1080p.jpg", "main10-av.mkv",
                                  "ld-musl-aarch64.so.1", "libdrm.so.2", "libudev.so.1"))
    stage(console, args, player_dir, ("vdp-player",))
    checked(console, "cd /tmp && chmod 755 lima-test jpgd-test vdp-player ld-musl-aarch64.so.1")
    for opts, jobs, expected_irq in (("-p 1 -n 16", 16, 32), ("-p 2 -n 16", 16, 32),
                                     ("-a -n 32", 32, 64)):
        before = checked(console, IRQS)
        out = checked(console, f"/tmp/lima-test {opts}", 50)
        after = checked(console, IRQS)
        delta = irq_delta(before, after, 126)
        if (out.count(b"4096/4096 pixels matched") != jobs or b"num_pp=2" not in out
                or b"LIMA GP+PP OFFSCREEN RENDER TEST: PASS" not in out or delta != expected_irq):
            raise RuntimeError(f"Lima regression with {opts}")
        console.record(f"[host] Lima {opts} matched {jobs}/{jobs} jobs, INTID 126 +{delta}\n".encode())

    before = checked(console, IRQS)
    checked(console, "/tmp/jpgd-test /dev/video1 1920 1080 /tmp/1080p.jpg")
    after = checked(console, IRQS)
    if irq_delta(before, after, 129) != 1:
        raise RuntimeError("Unexpected JPGD IRQ delta")
    console.record(b"[host] JPGD 1080p passed, INTID 129 +1\n")

    out = checked(console, "cd /tmp && ./ld-musl-aarch64.so.1 --library-path . "
                  "./vdp-player --osd main10-av.mkv", 85)
    playback = checked(console, IRQS)
    deltas = [irq_delta(after, playback, n) for n in (169, 122, 116)]
    if (b"V0 source=3840x2160" not in out or b"missed_vblanks=0 fps=60.000" not in out
            or b"PLAY decoded=1080 displayed=1080 dropped=0" not in out
            or b"ALSA played=864000 frames rate=48000 xruns=0" not in out
            or deltas[0] != 1080 or not all(n > 0 for n in deltas)):
        raise RuntimeError("VDEC/VDP/AIAO regression")
    console.record(f"[host] VDEC/VDP/AIAO IRQ deltas {deltas}, SMMU INTID 181=0\n".encode())

    out = checked(console, "dmesg", 45)
    if re.search(rb"SError|external abort|Kernel panic|SMMU (?:fault|error)\b|"
                 rb"SMMU faults=[1-9]|WARNING:|BUG:|"
                 rb"lima .*?(?:task error|time ?out|fault)|nobody cared|disabling IRQ", out, re.I):
        raise RuntimeError("Kernel log contains a fault after the regression")
    console.record(b"[host] Lima, JPGD, VDEC, VDP and AIAO regression passed\n")


def mesa_regression(console, args):
    stage(console, args, BUSYBOX, ("busybox.static", "mesa-runtime.tar.gz"))
    checked(console, "chmod 755 /tmp/busybox.static && /tmp/busybox.static mkdir -p /tmp/mesa && "
             "/tmp/busybox.static tar -xzf /tmp/mesa-runtime.tar.gz -C /tmp/mesa", 65)
    bound = []
    try:
        for name in ("dev", "sys", "proc"):
            checked(console, f"/tmp/busybox.static mount -o bind /{name} /tmp/mesa/{name}")
            bound.append(name)
        before = checked(console, IRQS)
        out = checked(console, "MESA_SHADER_CACHE_DISABLE=true "
                      "/tmp/busybox.static chroot /tmp/mesa /usr/local/bin/lima-gles", 60)
        after = checked(console, IRQS)
        delta = irq_delta(before, after, 126)
        # The shared GIC line may service more than one source per entry.
        # GLES checks the rendered pixels independently on all 60 frames.
        if (b"Mali450" not in out or b"LIMA_GLES_PASS frames=60 size=640x480" not in out
                or delta <= 0):
            raise RuntimeError("Mesa Lima GLES regression")
        console.record(f"[host] Mesa Lima GLES passed, INTID 126 +{delta}\n".encode())
        return delta
    finally:
        for name in reversed(bound):
            checked(console, f"/tmp/busybox.static umount /tmp/mesa/{name}")
