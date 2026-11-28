# HG680KD U-Boot USB Auto-Boot & Android Fallback

The current production image is `7.3.6-hg680kd-arm64-usb-final`. It adds native HDMI DRM and the AIAO drain fix to the USB boot path, with the matching `vdp-player` and musl MPV runtime installed on the F2FS root. The existing LBA 4136 read window still loads the padded legacy uImage. The verified prior image is available as `/boot/boot.img.bak`. [Current build, deployment and cold-boot evidence](USB-FINAL-ACCEPTANCE.md)

## Linux 7.3.6 full-media USB deployment

The HG680KD boots `7.3.6-hg680kd-arm64-usb` from the SanDisk USB disk. The board DTB keeps the F2FS root, `cma=384M`, USB storage and the disabled Linux eMMC node. It enables both Mali-450 PPs and PP MMUs, VDEC, JPGD, VENC, VDP and AIAO. VDP exposes G0 as the primary plane and V0 as the video overlay. The kernel includes KVM and the GIC maintenance and hypervisor timer interrupts. The stock BL31 enters Linux at EL1 and KVM reports `HYP mode not available` on this boot path. [Board DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-usb.dts) · [Resolved config](src/arm64-7.3.6-usb-full.config)

Run `bash scripts/build_usb_736.sh` from the project checkout on the build server. The script checks GCC 14.2.0, the KVM seed and the packaged Bluetooth firmware, then builds in `out/arm64-7.3.6-usb-full/clean.*/build/`. The 14,846,024-byte legacy uImage is padded to 15,245,384 bytes to match the allocated FAT file. The build completed with zero warnings. The USB player was rebuilt from the current source using the pinned FFmpeg request build, and the installed private libraries matched the build. [Kernel build](scripts/build_usb_736.sh) · [Player build](scripts/build_vdec_ffmpeg.sh)

| USB file | SHA256 |
| --- | --- |
| `/boot/boot.img` | `275642c781c1294e36d0ba4639eff98536d47b9cb77f26e56fbf65a0edd064ba` |
| `/boot/boot.img.bak` | `25b579f9dcc0399c71d5cc07620e4c3a8e81b1b7445ba4428b8ba4cf7eb13c3b` |
| `/opt/vdp-player/vdp-player` | `7d5efc7bbfd484a2f81bfca3151c05c4c7765dff6419ddb53c15a9cc521db2be` |

The install checked the USB transport and both partition UUIDs before writing. It preserved the active FAT file's allocation, synced and unmounted `/boot`, then read LBA 4136 through 33912 using direct I/O. The image payload matched byte for byte and the last 440 bytes matched the original window. A read-only remount verified both image hashes. The previous contents of `boot.img.bak` are available at `boot.img.bak.401fcc8c363c3640f172f5754cbe56698c30519fc85f69cff7a2c474708541e5`. To restore the immediately preceding image, use `/boot/boot.img.bak` with the in-place procedure below and restore its `boot.img` checksum entry. The host backup directory under `artifacts/arm64-7.3.6-usb-full/before/` also holds the previous player and manifests.

The system synced and unmounted before the operator disconnected and restored power. The operator confirmed the HDMI handoff. COM4 at 115200 8N1 recorded the shutdown and confirmed the installed hashes and USB mounts after reconnecting. The Windows serial handle dropped during the power cycle, so the cold-start BootROM output is absent. The framebuffer, Xfce, Mesa GLX/EGL, glmark2 and 32 alternating single- and dual-PP jobs passed. The cold media run verified HEVC 4K60 Main10 with G0 OSD and 48 kHz PCM, H.264, both MPEG formats, JPEG and FFmpeg `h264_v4l2m2m`. VENC SMMU faults and INTID 181 stayed at zero. DMA-BUF, CMA used pages and VENC CRG registers returned to their pre-test values after the media clients exited. [Cold-boot evidence](logs/539-usb-7.3.6-full.txt)

To repeat the cold media check, stage the test payload and its `TEST-SHA256SUMS` in a board tmpfs directory. Run `bash verify_usb_736.sh cold BOOT_SHA256 PLAYER_SHA256 CONFIG_SHA256` there as root, substituting the three artifact digests. It briefly stops LightDM for the KMS media tests, checks resource recovery, then restarts LightDM. The payload uses `lima-test`, `jpgd-test`, `1080p.jpg`, `vdec-mpeg-test`, both 1080p MPEG request fixtures, `main10-av.mkv`, `h264-1080p60.mp4`, `venc-regression.nv12`, the upstream AArch64 FFmpeg build and `venc-ioctl-smoke`. The generated `venc-usb.h264` passed host strict software decoding at 1920×1080 with 1 I frame and 11 P frames. [Board checks](scripts/verify_usb_736.sh) · [MPEG fixtures](VDEC-EXT-CODECS.md) · [FFmpeg build](VENC-ACCEPTANCE.md)

## Linux 7.3.6 dual-PP USB deployment

This USB build of `7.3.6-hg680kd-arm64-usb` enabled PP0, PP1 and both PP MMUs. Patch 0018 makes the broadcast interrupt mask follow the cores selected by each job and waits for the previous job's PP resets before programming the next mask. Patch 0019 narrows the interrupt mask to the unfinished cores when the shared handler observes a partial completion. The DT inherits the eight Mali interrupt entries on level-high SPI 94. GCC 14.2.0 compiled the image without warnings. The 15,245,384-byte image uses the F2FS root and 384 MiB CMA. [Diagnosis and RAM tests](LIMA-DUALPP.md) · [USB build](scripts/build_usb_736.sh)

| USB boot file | SHA256 |
| --- | --- |
| `/boot/boot.img` | `25b579f9dcc0399c71d5cc07620e4c3a8e81b1b7445ba4428b8ba4cf7eb13c3b` |
| `/boot/boot.img.bak-pre-dualpp` | `d298dcf14df298f0bd44dd4efd6efa07f5d343ebe31c754603cb8ea5e7f047b4` |

The TFTP preflight booted the new kernel with the existing Armbian F2FS root. U-Boot read back CRC32 `2e51caf2`. All 32 alternating one- and two-PP jobs matched 4,096 pixels, and three runs of the 1080p glmark2 build, texture and shading scenes completed on Mali450 with zero Lima timeouts. The installer staged the image in tmpfs and used `dd conv=notrunc,fsync` to update the FAT32 file in place. A direct read of LBA 4136 through 33912 matched the new file and preserved the 440 trailing bytes. The sector-window SHA256 is `d4f1ea87ae888cd705500c89979ca9683a53c1a88fa240e9e4b66d6656de7baa`.

COM4 captured a software reboot through the default U-Boot USB path. Linux mounted `/dev/sda3` as its F2FS root and `/dev/sda1` as VFAT. The installed kernel repeated the 32 alternating jobs and the glmark2 scenes with zero Lima timeouts. JPGD decoded a 1080p JPEG. VDEC and VDP displayed 1,080 2160p Main10 frames at 60.000 FPS with zero drops or missed vblanks, and AIAO played 864,000 PCM frames with zero XRUN. SMMU INTID 181 remained zero. [USB acceptance record](logs/533-usb-7.3.6-dualpp.txt)

For rollback use `/boot/boot.img.bak-pre-dualpp` and `/boot/BOOT-SHA256SUMS.bak-pre-dualpp` with the in-place restore procedure below. A verified host copy is in `artifacts/usb-backup/2026-11-28-pre-dualpp/`.

## Linux 7.3.6 Lima and native HDMI handoff

The first USB display build enabled Mali-450 with the CRG clocks and reset, shared SPI 94, and 200/300 MHz OPPs. Its internal MMUs used direct system DMA. VDP registered first as `card0`, and Lima provided `renderD128`. This build advertised PP0 and its MMU because a job using one core in a dual-PP topology could write back all pixels with no PP interrupt. Patch 0017 gave the broadcast handler sole ownership of PP completion on the shared IRQ and checked for pending PP work before counting idle cores. [Lima patch](patches/0017-drm-lima-handle-shared-pp-irq-once.patch)

The original VDP driver selected its tiled video plane as the KMS primary and omitted DRM fbdev client registration. USB now uses G0 as the RGB primary for fbcon, Xorg and Mesa kmsro. Its 1920×1080 KMS mode matches the firmware G0 canvas. On a 2160p60 firmware link, the original GP0 scaler expands G0 to the HDMI raster and the driver translates V0 destinations to physical HD0 pixels. The full-screen 3840×2160 tiled video still reaches V0 at unity, with G0 above it for alpha-blended OSD. Rebuild `vdp-player` with `scripts/build_vdec_ffmpeg.sh ~/hg680kd-mainline player` to discover V0 and G0 by supported format on both KMS layouts. [VDP driver](src/hi3798mv320-vdp.c) · [Player](src/vdp-player.c)

The build script at commit `7f263a6` uses the patched Linux 7.3.6 tree, GCC 14.2.0, the accepted KVM seed, and the two checked Bluetooth firmware files staged as described in the previous deployment below. It builds in `out/arm64-7.3.6-usb-display/` and writes the image, DTB, resolved `kernel.config` and SHA256 manifests to `build/deploy/`. The kernel retains the F2FS root parameters, `cma=384M`, and the 15,245,384-byte legacy image size.

The clean remote build produced the same kernel `Image` byte for byte with zero build warnings. In a pure RAM boot, both Mali render samples matched 4096/4096 pixels and the HDMI capture showed fbcon. The same kernel decoded a 1080p JPEG and displayed 1,080 HEVC Main10 frames at 60.000 FPS with 100 G0 OSD updates, zero dropped frames and zero missed vblanks. HDMI PCM played 864,000 frames at 48 kHz with zero XRUN. VDEC SMMU INTID 181 remained zero. With the final USB payload loaded via TFTP and the usual Armbian root, Xorg glamor, direct GLX, and 1080p glmark2 all used Mali450. The build, texture and shading tests and two LightDM restarts completed with zero Lima errors.

| USB boot file | SHA256 |
| --- | --- |
| `/boot/boot.img` | `d298dcf14df298f0bd44dd4efd6efa07f5d343ebe31c754603cb8ea5e7f047b4` |
| `/boot/boot.img.bak` | `401fcc8c363c3640f172f5754cbe56698c30519fc85f69cff7a2c474708541e5` |

The installer wrote the image in place on the FAT32 boot partition. It synced and unmounted before direct readback of LBA 4136 for 29,777 sectors. The bytes after the image in the final sector matched the original sector. A read-only remount confirmed both file hashes. Use the in-place rollback procedure below with `boot.img.bak` for this deployment.

After a physical power cycle, U-Boot used its default USB path and mounted the Armbian F2FS root. HDMI capture showed the Linux console, Xfce with hardware-rendered glxgears, and G0 OSD during Main10 playback. Direct GLX, EGL, and Xorg glamor selected Mali450. The Xorg log still records an indirect AIGLX load error for the VDP-named DRI2 driver. The cold regression verified VDEC, JPGD and HDMI PCM, with zero VDP underflows and zero interrupts on SMMU INTID 181. The stock BL31 boot still enters at EL1, and the USB configuration keeps KVM built in. [Cold-boot evidence](logs/523-usb-7.3.6-display.txt)

## 2026-11-28 initial Linux 7.3.6 USB deployment

The deployed kernel is `7.3.6-hg680kd-arm64-usb`, based on the validated 7.3.6 VDEC, VDP, AIAO and KVM RAM configuration. The USB build removes the RAM BusyBox initramfs and retains `root=PARTLABEL=HG680KD_ROOT`, `rootfstype=f2fs`, the original F2FS root flags, `rootwait` and `cma=384M`. The device tree inherits the tested GIC maintenance PPI 9 and virtual hypervisor timer PPI 12, and disables the Linux eMMC controller. The existing default U-Boot flow still reads stock BL31 into RAM. The deployment performed no OTP operation or `saveenv`. [USB device tree](src/hi3798mv320-fiberhome-hg680kd-arm64-usb.dts)

The uncompressed AArch64 legacy uImage is read into `0x01FDFFC0` with load and entry at `0x01FE0000`. Its embedded DTB starts at `0x01FE1000` and the kernel at `0x02000000`. The packed image occupies 14,846,024 bytes and is zero-padded to 15,245,384 bytes to preserve the existing FAT32 allocation. U-Boot reads 29,777 sectors starting at LBA 4136 (`0x1028`). The file and raw-sector readback matched the build byte for byte after sync, unmount and page-cache eviction. The last 440 bytes of the sector window matched the original window. [Readback and serial evidence](logs/519-usb-7.3.6-verification.txt)

| USB file | SHA256 |
|---|---|
| `/boot/boot.img` | `401fcc8c363c3640f172f5754cbe56698c30519fc85f69cff7a2c474708541e5` |
| `/boot/boot.img.bak-2026-11-28` | `670b1de4ff0c95df9738d18b5f1128b2ce3d42b555c9f9d5a4c8ee5840d45790` |

A verified copy of the original image is stored locally at `artifacts/usb-backup/2026-11-28/boot.img`. Both copies are 15,245,384 bytes. The active `BOOT-SHA256SUMS` records the new image, and its predecessor is stored at `/boot/BOOT-SHA256SUMS.bak-2026-11-28`. For rollback from a RAM system, confirm the USB partition identity and backup digest before mounting the FAT32 partition writable. Restore `boot.img` in place with `dd conv=notrunc,fsync` to preserve its LBA mapping, restore the checksum manifest and verify the restored file and sector window before rebooting.

COM4 at 115200 8N1 captured a software reboot through BootROM and the default USB boot command without TFTP. Armbian ran from `/dev/sda3` F2FS with 393,216 KiB CMA. `/dev/video0`, `/dev/media0`, `/dev/dri/card0` and `/dev/snd/pcmC0D0p` registered. All four CPUs entered at EL1 and KVM reported `HYP mode not available`. A physical power cycle was outside this check. The 2160p60 HEVC Main10 sample displayed 1,080/1,080 frames at 60.000 FPS with zero drops and zero missed vblanks. ALSA played 864,000 PCM frames at 48 kHz with zero XRUN, and SMMU INTID 181 remained zero. The player and its private libraries are installed under `/opt/vdp-player`, with `/opt/vdp-player/run` as the entry point. The existing `run-rpc_pipefs.mount` failure and systemd ordering cycle were also seen on 7.3.5.

The initial build used the patched Linux 7.3.6 tree, GCC 14.2.0 and the accepted `out/arm64-7.3.6-kvm-test` build under `~/hg680kd-mainline`. Stage `rtl8761btv_vat_fw.bin` and `rtl8761btv_vat_config.bin` from the original USB system's `/lib/firmware/rtl_bt/` into `out/arm64-7.3.6-usb-2026-11-28/firmware/rtl_bt/`. The builder checks and embeds both files for built-in Bluetooth probe before F2FS mounts. It disables global dynamic debug to fit the fixed read window. The initial builder is available at commit `4853d0f`. Its image and checksum were written to `out/arm64-7.3.6-usb-2026-11-28/deploy/`. The embedded kernel configuration records the absolute firmware directory, so the package hash depends on the build path.

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
