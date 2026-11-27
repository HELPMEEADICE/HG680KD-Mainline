# HG680KD U-Boot USB Auto-Boot & Android Fallback

## 2026-11-28 Linux 7.3.6 USB deployment

The deployed kernel is `7.3.6-hg680kd-arm64-usb`, based on the validated 7.3.6 VDEC, VDP, AIAO and KVM RAM configuration. The USB build removes the RAM BusyBox initramfs and retains `root=PARTLABEL=HG680KD_ROOT`, `rootfstype=f2fs`, the original F2FS root flags, `rootwait` and `cma=384M`. The device tree inherits the tested GIC maintenance PPI 9 and virtual hypervisor timer PPI 12, and disables the Linux eMMC controller. The existing default U-Boot flow still reads stock BL31 into RAM. The deployment performed no OTP operation or `saveenv`. [USB device tree](src/hi3798mv320-fiberhome-hg680kd-arm64-usb.dts)

The uncompressed AArch64 legacy uImage is read into `0x01FDFFC0` with load and entry at `0x01FE0000`. Its embedded DTB starts at `0x01FE1000` and the kernel at `0x02000000`. The packed image occupies 14,846,024 bytes and is zero-padded to 15,245,384 bytes to preserve the existing FAT32 allocation. U-Boot reads 29,777 sectors starting at LBA 4136 (`0x1028`). The file and raw-sector readback matched the build byte for byte after sync, unmount and page-cache eviction. The last 440 bytes of the sector window matched the original window. [Readback and serial evidence](logs/519-usb-7.3.6-verification.txt)

| USB file | SHA256 |
|---|---|
| `/boot/boot.img` | `401fcc8c363c3640f172f5754cbe56698c30519fc85f69cff7a2c474708541e5` |
| `/boot/boot.img.bak-2026-11-28` | `670b1de4ff0c95df9738d18b5f1128b2ce3d42b555c9f9d5a4c8ee5840d45790` |

A verified copy of the original image is stored locally at `artifacts/usb-backup/2026-11-28/boot.img`. Both copies are 15,245,384 bytes. The active `BOOT-SHA256SUMS` records the new image, and its predecessor is stored at `/boot/BOOT-SHA256SUMS.bak-2026-11-28`. For rollback from a RAM system, confirm the USB partition identity and backup digest before mounting the FAT32 partition writable. Restore `boot.img` in place with `dd conv=notrunc,fsync` to preserve its LBA mapping, restore the checksum manifest and verify the restored file and sector window before rebooting.

COM4 at 115200 8N1 captured a software reboot through BootROM and the default USB boot command without TFTP. Armbian ran from `/dev/sda3` F2FS with 393,216 KiB CMA. `/dev/video0`, `/dev/media0`, `/dev/dri/card0` and `/dev/snd/pcmC0D0p` registered. All four CPUs entered at EL1 and KVM reported `HYP mode not available`. A physical power cycle was outside this check. The 2160p60 HEVC Main10 sample displayed 1,080/1,080 frames at 60.000 FPS with zero drops and zero missed vblanks. ALSA played 864,000 PCM frames at 48 kHz with zero XRUN, and SMMU INTID 181 remained zero. The player and its private libraries are installed under `/opt/vdp-player`, with `/opt/vdp-player/run` as the entry point. The existing `run-rpc_pipefs.mount` failure and systemd ordering cycle were also seen on 7.3.5.

To reproduce the image, start with the patched Linux 7.3.6 source tree, GCC 14.2.0 toolchain, and accepted `out/arm64-7.3.6-kvm-test` build under `~/hg680kd-mainline`. Stage `rtl8761btv_vat_fw.bin` and `rtl8761btv_vat_config.bin` from the original USB system's `/lib/firmware/rtl_bt/` into `out/arm64-7.3.6-usb-2026-11-28/firmware/rtl_bt/`. The build script checks their hashes and embeds them so the built-in Bluetooth driver can probe before the F2FS root is mounted. It disables global dynamic debug to fit the fixed read window. Run `bash scripts/build_usb_736.sh` from the project checkout. The packed image and its checksum are written under `out/arm64-7.3.6-usb-2026-11-28/deploy/`. The embedded kernel configuration records the absolute firmware directory, so the package hash depends on the build path. [Build script](scripts/build_usb_736.sh)

## 2026-11-22 USB VDEC deployment

That deployment used `7.3.5-hg680kd-arm64-armbian-usb-vdec`. It includes the validated H.264 Direct fix and Cacheable + NEON detiling from `arm64-7.3.5-vdec-direct`. The legacy AArch64 uImage includes the board DTB and boots without an initrd. It retains `root=PARTLABEL=HG680KD_ROOT`, `rootfstype=f2fs` and the existing F2FS mount options, and adds `cma=384M`.

The build on `hedc@ArchLinux-E5-Server` disables global dynamic debug to fit the existing U-Boot read window. The VDEC driver and media dependencies are installed under `/lib/modules/7.3.5-hg680kd-arm64-armbian-usb-vdec/`. The final file is padded to 15,245,384 bytes and written in place to preserve LBA `0x1028` and the existing `0x7451`-sector read. File and raw-sector readback matched the build after sync and unmount.

| USB file | SHA256 |
|---|---|
| `/boot/boot.img` | `670b1de4ff0c95df9738d18b5f1128b2ce3d42b555c9f9d5a4c8ee5840d45790` |
| `/boot/boot.img.bak-pre-vdec` | `f5e3173703253ff79cd4c9e329c9b57856a42e7cbdb745656c19ed8288f67b70` |

The original `Image`, DTB and checksum manifest also have `.bak-pre-vdec` copies. Restore `boot.img` in place with `dd conv=notrunc,fsync` to preserve its sector mapping.

Two COM4-triggered software reboots followed the normal USB path and reached the Armbian serial login. The root mounted from `/dev/sda3`, CMA reserved 384 MiB, and `/dev/video0` and `/dev/media0` registered. H.264 1080p with default Direct, HEVC 4K Main, and HEVC 4K Main10 each matched 240/240 software-decoded frame hashes, with 720 PXP interrupts and zero SMMU interrupts. The pre-existing `run-rpc_pipefs.mount` failure also appeared on the original USB kernel. eMMC stayed read-only and the deployment performed no OTP operation or `saveenv`.

## 1. Original Stock U-Boot Environment (`printenv` Snapshot)

Captured unmodified from COM4 (`115200 8N1`) before any environment changes ([logs/370-uboot-printenv.txt](logs/370-uboot-printenv.txt) and [logs/387-uboot-usb-probe.txt](logs/387-uboot-usb-probe.txt)):

```text
api_address=4fe818
baudrate=115200
bootargs=androidboot.hardware=bigfish androidboot.selinux=permissive androidboot.serialno=0010069900E068A00925540A7788B72C androidboot.product.model=FIBERHOME_HG680KD androidboot.product.hardware=HG680-ND8XX-1N1 androidboot.product.model=FIBERHOME_HG680KD androidboot.product.hardware=HG680-ND8XX-1N1 console=ttyAMA0,115200 blkdevparts=mmcblk0:2M(fastboot),512K(bootargs),512K(bootargsbak),19M(recovery),2M(deviceinfo),8M(securestore),2M(atf),8M(baseparam),8M(pqparam),2M(dtbo),10M(logo),10M(logobak),20M(dfx),20M(recoverybak),60M(boot),20M(misc),20M(trustedcore),1200M(system)ro,800M(cache),400M(vendor)ro,800M(backup),50M(private),256M(ctc),-(userdata) hbcomp=/dev/block/mmcblk0p18 init=/init root=/dev/mmcblk0p18 androidboot.dtbo_idx=0 pq=noacmuhd quiet nofmc nosd disp_offset_vfs=disable rootwait androidboot.hardware.lsadcix=11 androidboot.hardware.boardid=4 mntnctl=010010111100101010111111101001100000011000001 last_bootup_keypoint=30 reboot_reason=AP_S_AWDT exception_subtype=SUB_APWDT_AP
bootargs_1G=mem=1G mmz=ddr,0,0,52M vmalloc=500M
bootargs_2G=mem=2G mmz=ddr,0,0,52M vmalloc=500M
bootargs_3840M=mem=3840M mmz=ddr,0,0,48M vmalloc=500M
bootargs_3G=mem=3G mmz=ddr,0,0,52M vmalloc=500M
bootargs_512M=mem=512M mmz=ddr,0,0,32M vmalloc=500M
bootargs_768M=mem=768M mmz=ddr,0,0,32M vmalloc=500M
bootcmd=mmc read 0 0x8020000 0x10000 0x1000;mmc read 0 0x1FFBFC0 0x38000 0x7800;bootm 0x1FFBFC0
bootdelay=3
bootfile="uImage"
ethact=gmac0
ethaddr=54:0A:77:88:B7:2C
gmac_debug=0
ipaddr=192.168.1.10
netmask=255.255.255.0
phy_addr=2,1
phy_intf=mii,rgmii
recoverybootargs=androidboot.hardware=bigfish androidboot.selinux=permissive androidboot.serialno=0123456789 console=ttyAMA0,115200 blkdevparts=mmcblk0:2M(fastboot),512K(bootargs),512K(bootargsbak),19M(recovery),2M(deviceinfo),8M(securestore),2M(atf),8M(baseparam),8M(pqparam),2M(dtbo),10M(logo),10M(logobak),20M(dfx),20M(recoverybak),60M(boot),20M(misc),20M(trustedcore),1200M(system),800M(cache),400M(vendor),800M(backup),50M(private),256M(ctc),-(userdata) hbcomp=/dev/block/mmcblk0p18 init=/init root=/dev/mmcblk0p18 androidboot.dtbo_idx=0 pq=noacmuhd
serverip=192.168.1.1
stderr=serial
stdin=serial
stdout=serial
use_mdio=0,1
ver=U-Boot 2022.07-g80522c56-dirty (Jun 05 2025 - 21:16:17 +0800)
verify=n

Environment size: 2292/65532 bytes
```

## 2. Vendor U-Boot Hardware Findings

1. **Simple CLI (`cli_simple`) without Hush**: This U-Boot build does not enable `CONFIG_HUSH_PARSER` (`if/then/else`, `&&`, `||`, `test`, `part`, and `load` are unsupported, verified in [logs/384-uboot-shell-test.txt](logs/384-uboot-shell-test.txt) and [logs/377-uboot-commands.txt](logs/377-uboot-commands.txt)). Commands separated by `;` execute sequentially; if USB `bootm 0x1fdffc0` fails, execution falls straight through to `;run stock_bootcmd`.
2. **Low `sp` vs `ram_top` LMB constraint on `fatload`**: `bdinfo` shows HiSilicon U-Boot relocated at `relocaddr = 0x00b0c000` with `sp start = 0x004fbec0` while `DRAM size = 0x80000000` (`2 GiB`). U-Boot 2022.07 `fs_read_lmb_check()` reserves `[sp - 4096 .. ram_top]`, causing `fatload` at any address `>= 0x00500000` (such as `0x01fdffc0`, `0x10000000`, `0x70000000`) to abort with `** Reading file would overwrite reserved memory **` ([logs/388-uboot-fatload-addrs.txt](logs/388-uboot-fatload-addrs.txt), [logs/388-uboot-lmb-bounds.txt](logs/388-uboot-lmb-bounds.txt)). Meanwhile, `usb read` bypasses `fs_read_lmb_check` (matching how stock `bootcmd` uses `mmc read`). On the freshly formatted `HG680KD_BOOT` FAT32 partition (start LBA `2048`), `boot.img` (`15,245,384` bytes, SHA256 `f5e31737...`) is stored contiguously starting at disk **LBA `4136` (`0x1028`)** for **`29,777` sectors (`0x7451`)**, verified via `crc32 1fdffc0 e8a048 ==> b4cfc0a7` ([logs/388-uboot-usbread-test.txt](logs/388-uboot-usbread-test.txt)). Calling `usb stop` right after `usb read` cleanly halts U-Boot's xHCI controller before `bootm`.
3. **Internal FEPHY / GMAC clocks on non-TFTP boot**: When U-Boot boots without first running a network command (`tftpboot`/`ping`), four CRG registers remain gated/reset (`0xf8a220cc=0x20`, `0xf8a220d0=0x100001`, `0xf8a22388=0x11`, `0xf8a223bc=0x1`, and `0xf984300c=0`), leaving internal FEPHY (`0x20669908` at PHYAD `1`) unresponsive (`phy_id=0x00000000` in Linux). Writing the minimal diff (`fephy_init`, identified in [logs/394d-uboot-full-diff.txt](logs/394d-uboot-full-diff.txt) and verified in [logs/394e-uboot-4reg-fephy-verify.txt](logs/394e-uboot-4reg-fephy-verify.txt) and [logs/396-armbian-fephy-live-check.txt](logs/396-armbian-fephy-live-check.txt)) wakes FEPHY immediately without network timeout delays.

## 3. Configured Environment Diff & Fallback Chain

Executed and persisted via `saveenv` ([logs/398-uboot-saveenv.txt](logs/398-uboot-saveenv.txt)) after verifying both manual USB Armbian boot ([logs/389-usb-manual-armbian-boot.txt](logs/389-usb-manual-armbian-boot.txt), [logs/395-armbian-with-fephy-boot.txt](logs/395-armbian-with-fephy-boot.txt)) and fallback paths ([logs/397-uboot-fallback-verify.txt](logs/397-uboot-fallback-verify.txt)):

```text
# Before:
bootcmd=mmc read 0 0x8020000 0x10000 0x1000;mmc read 0 0x1FFBFC0 0x38000 0x7800;bootm 0x1FFBFC0

# After (Environment size: 2588/65532 bytes; bootargs & all other stock variables unchanged):
fephy_init=mw.l f8a220cc 0;mw.l f8a220d0 100010;mw.l f8a22388 1;mw.l f8a223bc 10;mw.l f984300c 1f
stock_bootcmd=mmc read 0 0x8020000 0x10000 0x1000;mmc read 0 0x1FFBFC0 0x38000 0x7800;bootm 0x1FFBFC0
usb_bootcmd=run fephy_init;usb start;mw.l 0x1fdffc0 0 1;usb read 0x1fdffc0 0x1028 0x7451;usb stop;mmc read 0 0x8020000 0x10000 0x1000;bootm 0x1fdffc0
bootcmd=run usb_bootcmd;run stock_bootcmd
```

### Fallback Guarantee
1. Before every USB read, `mw.l 0x1fdffc0 0 1` clears any stale Legacy uImage magic at `0x01fdffc0`.
2. If no USB storage device is present or the plugged-in USB device does not carry a valid Legacy uImage at `LBA 0x1028`, `bootm 0x1fdffc0` rejects the header (`Wrong Image Format for bootm command`) and `bootcmd` immediately proceeds to `run stock_bootcmd`.
3. `stock_bootcmd` reloads ATF BL31 (`0x08020000`) and stock Android `boot` (`0x01FFBFC0`, overwriting any transient USB read buffer) cleanly from eMMC.
