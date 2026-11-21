# 烽火 HG680KD 主线 Linux 适配

本项目以 Linux 6.18.64 为基线，从原厂 U-Boot 2022.07 和 BL31 启动 RAM 镜像。设备为海思 Hi3798MV320，四核 Cortex-A55，实装 2 GiB LPDDR4。核心与外设镜像均经 COM4 实机验证，带 BusyBox 1.37.0 和静态链接的 musl 1.2.5 用户态。

## ARM64 2 GiB HDMI

`arm64-all-2g-hdmi` 从原厂 U-Boot 接管 HDMI 图形层。实测显存 `0x7f6c1000–0x7fea9fff`，1920×1080，pitch 7680 字节，`a8r8g8b8`。原厂 1280×720 日志对应 Logo JPEG 解码，接入本次显示设备时，HDMI AVI InfoFrame 声明 3840×2160p60。Linux 6.18.64 RAM 启动后注册 simpledrm `/dev/dri/card0`、`/dev/fb0` 和帧缓冲控制台，Mali-450 Lima `/dev/dri/renderD128` 两次离屏渲染均为 4096/4096 像素匹配。采集卡确认控制台和八色彩条上屏。同次验证还通过四核 SMP、1840 MiB 内存测试、USB FAT 只读挂载、eMMC `ro=1` 与原厂 boot 容器全量 SHA256、有线 ICMP 和 `fastfetch`。[寄存器与原生 DRM 评估](HDMI-REVERSE.md) · [现场寄存器](logs/226-hdmi-uboot-registers.txt) · [最终串口](logs/249-mainline-arm64-all-2g-hdmi-final.txt) · [HDMI 采集帧](artifacts/arm64-all-2g-hdmi/hdmi-color-bars.png)

| 用途 | RAM 镜像 | SHA256 |
| --- | --- | --- |
| HDMI | [arm64-all-2g-hdmi/boot.img](artifacts/arm64-all-2g-hdmi/boot.img) | `52083ae85db4a9dadfb8f0350322f8ee2fe5f5d1729569a27cf91fbede966f20` |
| 6.18.64 回退 | [arm64-all-2g/uImage](artifacts/arm64-all-2g/uImage) | `4196860c3e7f499f286661f929c237d5549cb800313b84ec5306c9209228e85d` |

同步 `src/`、`patches/` 与构建脚本后，在服务器构建并取回 `out/release-arm64-all-2g-hdmi.tar.gz`。

```bash
root="$HOME/hg680kd-mainline"
bash scripts/build.sh full "$root" "$root/inputs" arm64-all-2g-hdmi
```

把归档解包到 `artifacts/arm64-all-2g-hdmi`，使用相同的 ARM64 封装器。该目标的 `boot.img` 是 Legacy ARM64 uImage，使用 `bootm 0x1fdffc0` 引导。[镜像 SHA256 清单](artifacts/arm64-all-2g-hdmi/BOOT-SHA256SUMS)

```powershell
$out = 'artifacts/arm64-all-2g-hdmi'
python scripts/pack_arm64.py --image "$out/Image" `
  --dtb "$out/hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi.dtb" `
  --out $out --name boot.img
```

HDMI 设备应在盒子上电前接好，使 U-Boot 建立有效的显示链路。复用下文 ARM64 的 TFTP 或 XMODEM RAM 引导方法，把 `--image` 改为本节的镜像路径。TFTP 文件名应与服务端一致。以下命令在 BusyBox 中绘制彩条，并避免屏幕日志和光标覆盖画面。simpledrm 沿用 U-Boot 模式，原生改分辨率和热插拔仍属后续驱动工作。

```sh
echo N >/sys/module/printk/parameters/ignore_loglevel
dmesg -n 4
echo 0 >/sys/class/graphics/fbcon/cursor_blink
printf '\033[?25l' >/dev/tty1
fb-test
```

## ARM64 HDMI 无线探测

本机 HG680KD 的原厂标识为 `HG680-ND8XX-1N1`，`lsadcix=11`、`boardid=4`。U-Boot 参数与 GPIO19/20/21 绑带现场读数一致。原厂 `init.fhservice.sh` 将该板选为 `ro.fh.ddr4.btonly`（单蓝牙硬件版本 `9900E00100062401ND0XX1L1`）。通用 `vendor` 镜像包含多套 Wi-Fi 驱动，SDIO 和两路原厂 USB 控制器的实测枚举均未发现板载 Wi-Fi。详见 [板型、总线与上电证据](WIFI-REVERSE.md)。

独立的 [arm64-all-2g-hdmi-wifi-probe/boot.img](artifacts/arm64-all-2g-hdmi-wifi-probe/boot.img) SHA256 为 `f57038eb565ef04366282a7b0a6ed2ac53fd3258df5361684a1a4cf5b4d8276d`。该镜像复现原厂 SDIO 时钟、复位、采样相位和 GPIO4_3 上电条件，`CMD5` 仍返回 `-110`，无 SDIO ID 或 `wlan0`。从 RAM 启动后验证了 HDMI simpledrm 和采集卡彩条、Lima 离屏渲染、四核 SMP、1840 MiB 内存读写、USB FAT 只读挂载、有线网络和 `fastfetch`。eMMC 整盘及两个分区的 `ro` 均为 1，`system`、`vendor` 通过 ext4 `ro,noload` 挂载。[完整串口](logs/267-mainline-arm64-hdmi-wifi-final.txt) · [原厂板型分类](logs/270-wifi-stock-btonly-selection.txt) · [HDMI 采集校验](logs/273-wifi-image-hdmi-capture.txt)

在同一构建服务器同步 `src/`、`patches/` 与构建脚本后执行：

```bash
root="$HOME/hg680kd-mainline"
bash scripts/build.sh full "$root" "$root/inputs" arm64-all-2g-hdmi-wifi-probe
```

取回 `out/release-arm64-all-2g-hdmi-wifi-probe.tar.gz` 并解包到 `artifacts/arm64-all-2g-hdmi-wifi-probe`，按 HDMI 目标相同方式使用 [pack_arm64.py](scripts/pack_arm64.py) 封装该目录中的 `Image` 与 `hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi-wifi-probe.dtb`。TFTP 传输和板上 CRC32 校验后执行 `bootm 0x1fdffc0`。

## 其他已验证镜像

建议使用 [全功能合并镜像](artifacts/arm32-all/boot.img)（`arm32-all`，128 MiB 基线）或 [2 GiB 内存扩容全功能镜像](artifacts/arm32-all-2g/boot.img)（`arm32-all-2g`，含 `fastfetch`）。同一次 RAM 启动验证了四核在线、24 MHz 架构定时器、USB 480 Mb/s FAT 只读挂载、eMMC 8-bit 52 MHz 高速 ADMA2 只读、有线网卡双向 ICMP/HTTP TCP 传输，以及 Mali-450 主线 Lima 驱动绑定与 GP+PP 离屏渲染写回。`arm32-all-2g` 进一步在启用 `CONFIG_HIGHMEM=y` 与 `CONFIG_BOUNCE=y` 后开放完整 2 GiB 物理窗口（实测可用 `MemTotal = 2,051,208 KiB` ≈ `2003.1 MiB`，含 `LowTotal = 750,220 KiB` 与 `HighTotal = 1,300,988 KiB`），并集成 `fastfetch 2.71.0`、BusyBox `free` 与 `memtest-2g`，通过 1,872 MiB 跨高低端物理内存读写校验（覆盖 `0x00000000–0x7f5ff000`）与全部外设回归。见 [175-mainline-all.txt](logs/175-mainline-all.txt)、[176-server-ping-all.txt](logs/176-server-ping-all.txt)、[195-mainline-all-2g-r2-peripherals.txt](logs/195-mainline-all-2g-r2-peripherals.txt) 和 [202-mainline-all-2g-fastfetch.txt](logs/202-mainline-all-2g-fastfetch.txt)。

已验证的低速存储合并回退镜像保留在 [artifacts/arm32-storage/boot.img](artifacts/arm32-storage/boot.img)（400 kHz、1-bit、PIO 只读 eMMC + USB），低速独立 eMMC 镜像保留在 [artifacts/arm32-emmc-final/boot.img](artifacts/arm32-emmc-final/boot.img)，均未覆盖。

## 实机结果

| 项目 | 证据 |
| --- | --- |
| RAM 启动 | 原厂 BL31 AArch32 warm reset 后，Linux 识别板级 DT，并在 COM4 115200 8N1 上进入 BusyBox shell。见 [17-mainline-timer-validation.txt](logs/17-mainline-timer-validation.txt)。 |
| CPU 和中断 | PSCI 1.1/SMC，GICv3 四个 redistributor，对应 MPIDR `0x000/0x100/0x200/0x300`。用户态 online 为 `0-3`，各核 MIDR part 为 `0xd05`。见 [12-live-shell-and-timer.txt](logs/12-live-shell-and-timer.txt)。 |
| 定时器 | 24 MHz CP15 虚拟定时器，clocksource 为 `arch_sys_counter`。修正 `CONFIG_COMPAT_32BIT_TIME` 后，`sleep 2` 的主机观测耗时 2.02 秒，板上 uptime 增加 2.00 秒，各核 PPI 27 增加约 200 次。见 [17-mainline-timer-validation.txt](logs/17-mainline-timer-validation.txt)。 |
| 传输 | XMODEM-1K 921600 及 TFTP 均通过板上 CRC32 与本地比对。见 [17-mainline-timer-validation.txt](logs/17-mainline-timer-validation.txt) 和 [26-tftp-probe.txt](logs/26-tftp-probe.txt)。 |
| eMMC 只读（逐步提速） | Nebula `0xf9830000` 始终保持 `ro=1` 并屏蔽原始 MMC ioctl。从已保留的 400 kHz / 1-bit / PIO 基线出发，逐项验证了 8-bit 总线宽度、52 MHz（实测 48 MHz）`mmc high-speed` 时钟与相位、SDMA 及 ADMA2，每步均通过原厂 boot 容器首尾各 64 KiB SHA256 回归；高速与 DMA 阶段并额外通过完整 12,304,384 字节原厂 `boot.img` SHA256 校验。见 [148-mainline-emmc-w8.txt](logs/148-mainline-emmc-w8.txt)、[152-mainline-emmc-hs.txt](logs/152-mainline-emmc-hs.txt)、[156-mainline-emmc-sdma.txt](logs/156-mainline-emmc-sdma.txt) 和 [160-mainline-emmc-adma.txt](logs/160-mainline-emmc-adma.txt)。 |
| USB 存储 | 主线 xHCI 以 480 Mb/s 枚举 SanDisk U 盘 `0781:5591`，并以只读模式挂载 `/dev/sda1`。从 FAT 分区读取完整原厂 `boot.img` 和 `factory.dtb`，SHA256 都与 Windows 上的备份一致。见 [109-mainline-usb-phy-storage.txt](logs/109-mainline-usb-phy-storage.txt) 和 [111-usb-speed-and-original-sha256.txt](logs/111-usb-speed-and-original-sha256.txt)。 |
| 有线网络 | GMAC `0xf9840000`（`MAC_SET[29:28]=3`）切换为主线 `GEMAC_V2`（`HW_CAP_TSO`）16 字节描述符命令格式后修复 TX。实测 100 Mb/s 全双工，板端与编译服务器/网关双向 ping（56 B 与 1472 B MTU 1500）0% 丢包、RTT 0.16–0.48 ms，服务端 ARP 为 `REACHABLE`；合并镜像中通过 HTTP/TCP `wget` 拉取 3.22 MiB 镜像（2394 RX / 1301 TX 帧，跨 1024 描述符环回绕），SHA256 逐字节一致。见 [142-mainline-wired-r5.txt](logs/142-mainline-wired-r5.txt)、[143-server-ping-net-r5.txt](logs/143-server-ping-net-r5.txt)、[144-net-r5-mtu-and-reboot.txt](logs/144-net-r5-mtu-and-reboot.txt) 和 [175-mainline-all.txt](logs/175-mainline-all.txt)。 |
| Mali-450 GPU | 核对原厂 DT、内核反汇编与 U-Boot 寄存器后，在 CRG `0xf8a22000` 补充 GPU 时钟门控（`0xd4` 位 `0/8/9`）、频率回读（`0x124[2:0]=7` 对应 200 MHz）和复位（`0xd4` 位 `4`），未修改主线 Lima 驱动即成功绑定 `arm,mali-450`（1× GP + 2× PP + 8K/64K L2 Cache，8 路共享 GIC SPI 94 中断），注册 `/dev/dri/renderD128` 与 `/dev/dri/card0`。静态工具 `lima-test` 连续执行两次完整 GP（PLBU）+ DLBU + 双 PP 64×64 RGBA8888 离屏清屏写回，4096/4096 像素全部精确匹配（`0xff336699` / `0xffaabbcc`），共享中断正常触发且内核零错误。见 [138-uboot-gmac-gpu-emmc-regs.txt](logs/138-uboot-gmac-gpu-emmc-regs.txt)、[170-mainline-gpu-lima-gp-pp.txt](logs/170-mainline-gpu-lima-gp-pp.txt) 和 [175-mainline-all.txt](logs/175-mainline-all.txt)。 |
| 板载无线 | 本机 `boardid=4` 对应原厂单蓝牙版本。在 ARM64 上验证 `0xf9c40000` SDIO 的上电及主机初始化，CMD5 超时 `-110`，无线接口未出现。见 [无线逆向记录](WIFI-REVERSE.md) 和 [ARM64 串口](logs/267-mainline-arm64-hdmi-wifi-final.txt)。 |
| `fastfetch` 集成 | `all-fastfetch` 目标在全功能镜像中集成静态链接 musl 1.2.5 的 `fastfetch 2.71.0`（关闭 X11/Wayland/Vulkan/OpenCL/DBus/libdrm 等重型依赖并剥离符号，二进制 2.0 MiB，`boot.img` 共 4,227,072 字节），配套 `/etc/os-release`、`/etc/hostname`（`hg680kd`）、`/etc/passwd` 与 `/etc/fastfetch/config.jsonc`。上板实机验证默认输出（0.12 s）及 `--pipe` 无复杂转义模式（0.06 s）均清晰无误，完整展示 `OS`、`Host: FiberHome HG680KD`、`Kernel`、`CPU: hi3798mv320 (4)`、`GPU: Arm mali-450 [Integrated]`、`Memory`、`Disk (/)` 与 `Local IP (eth0): 192.168.6.249/24`，同时 `lima-test` 离屏渲染通过。见 [180-mainline-all-fastfetch.txt](logs/180-mainline-all-fastfetch.txt)。 |
| ARM64（AArch64）最小化内核交接 | 反汇编原厂 `fastboot` 与 `atf` 确认 U-Boot 运行于 AArch32 EL3、BL31 运行于 AArch64 EL3，且 `bootm` 原生支持 `IH_ARCH_ARM64`（`#22`），通过 `start_atf(64, ...)` 设 `bl33_ep->spsr = 0x3C5`（AArch64 EL1h）并写 `RMR` Warm Reset 切入 BL31。使用 [pack_arm64.py](scripts/pack_arm64.py) 封装 32 B AArch64 DTB 入口 shim + 极简 ARM64 DTB + 2 MiB 对齐主线 Linux 6.18.64 `Image`（[artifacts/arm64-core/uImage](artifacts/arm64-core/uImage)，已内置静态 AArch64 `fastfetch 2.71.0`），纯 RAM TFTP 引导后四核 Cortex-A55 全在线（`CPU: All CPU(s) started at EL1`）、24 MHz 定时器正常、进入 AArch64 BusyBox 1.37.0 shell（`uname -a` 输出 `aarch64`，`fastfetch` 实测 0.06 s 正常输出）。详析见 [ARM64-HANDOFF.md](ARM64-HANDOFF.md)，实机日志见 [210-mainline-arm64-core.txt](logs/210-mainline-arm64-core.txt)、[211-mainline-arm64-cpuinfo.txt](logs/211-mainline-arm64-cpuinfo.txt) 和 [214-mainline-arm64-fastfetch.txt](logs/214-mainline-arm64-fastfetch.txt)。 |
| ARM64 全功能 + 2 GiB 扩容 | 在 [hi3798mv320-fiberhome-hg680kd-arm64-all-2g.dts](src/hi3798mv320-fiberhome-hg680kd-arm64-all-2g.dts) 开放完整 2 GiB 物理窗口（`0x00000000–0x7fffffff`）并以 5 处 `no-map` 保留区保护固件与引导内存（实测 `MemTotal = 2,025,420 KiB` ≈ `1.93 GiB`，`memtest-2g` 验证 1,856 MiB 覆盖 `0x01400000–0x7ccff000`），合入 USB xHCI、Nebula eMMC（8-bit 52 MHz HS ADMA2，`ro=1`）、GMAC（修复 ARM64 下 `NET_IP_ALIGN=0` 导致的 RX 2 字节偏移问题，实测 100 Mb/s 双向 56 B / 1472 B ICMP 0% 丢包与 11.38 MiB HTTP/TCP `wget` SHA256 一致）及 Mali-450 Lima（`/dev/dri/renderD128`，AArch64 静态 `lima-test` 两次 4096/4096 像素匹配、GIC SPI 94 中断计数正常、`fastfetch` 识别 `GPU: Arm mali-450 [Integrated]`）。产物见 [artifacts/arm64-all-2g/uImage](artifacts/arm64-all-2g/uImage)，实机日志见 [218-mainline-arm64-all-2g-full.txt](logs/218-mainline-arm64-all-2g-full.txt) 和 [219-server-ping-arm64-all-2g.txt](logs/219-server-ping-arm64-all-2g.txt)。 |
| ARM64 Linux 7.2.5 升级 | 将 ARM64 全功能补丁集（`0002`–`0005`）、DTS 及自研驱动前向移植至主线 **Linux 7.2.5**（`aarch64-linux-gcc 14.2.0` 零警告通过编译；`scripts/build.sh` 与 `scripts/build_arm64.sh` 新增 `arm64-7.2.5-all-2g` 目标并保留既有 6.18.64 目标）。经检查，`MMC_CAP2_FORCE_READ_ONLY`（bit 29）、`xhci_plat_probe`/`xhci_plat_remove`、`sdhci_pltfm_*`、`hisi_reset_*` 及 `hix5hd2_gmac` 接口在 6.18 → 7.2 间保持兼容，无需额外破坏性 API 修改。单次 TFTP 纯 RAM 引导完整通过四核 SMP、`MemTotal = 2,025,216 KiB`（~1.93 GiB）与 `memtest-2g` 1,840 MiB 校验、USB 480 Mb/s FAT 只读挂载、eMMC 8-bit 52 MHz HS ADMA2（`ro=1`）全量 `boot.img` SHA256 回归、GMAC 100 Mb/s 双向 ICMP 0% 丢包与 11.56 MB HTTP `wget` SHA256 一致、Mali-450 Lima 两次 4096/4096 离屏渲染及 `fastfetch` 输出。产物见 [artifacts/arm64-7.2.5-all-2g/uImage](artifacts/arm64-7.2.5-all-2g/uImage)，日志见 [223-mainline-arm64-7.2.5-all-2g-full.txt](logs/223-mainline-arm64-7.2.5-all-2g-full.txt) 和 [224-server-ping-arm64-7.2.5-all-2g.txt](logs/224-server-ping-arm64-7.2.5-all-2g.txt)。 |

核心镜像位于 [artifacts/arm32-timer-fix/boot.img](artifacts/arm32-timer-fix/boot.img)。已保留的低速回退镜像为 [artifacts/arm32-storage/boot.img](artifacts/arm32-storage/boot.img) 与 [artifacts/arm32-emmc-final/boot.img](artifacts/arm32-emmc-final/boot.img)。本次新增的独立与合并验证镜像包括：
- 全功能合并镜像（128 MiB 基线保留回退）：[artifacts/arm32-all/boot.img](artifacts/arm32-all/boot.img)
- 全功能合并镜像（2 GiB 内存扩容）：[artifacts/arm32-all-2g/boot.img](artifacts/arm32-all-2g/boot.img)
- 全功能合并镜像（含 `fastfetch`）：[artifacts/arm32-all-fastfetch/boot.img](artifacts/arm32-all-fastfetch/boot.img)
- 有线网络双向连通镜像：[artifacts/arm32-net-r5/boot.img](artifacts/arm32-net-r5/boot.img)
- eMMC 逐级提速镜像：[arm32-emmc-w8](artifacts/arm32-emmc-w8/boot.img)（8-bit 400 kHz PIO）、[arm32-emmc-hs](artifacts/arm32-emmc-hs/boot.img)（8-bit 52 MHz HS PIO）、[arm32-emmc-sdma](artifacts/arm32-emmc-sdma/boot.img)（8-bit 52 MHz HS SDMA）、[arm32-emmc-adma](artifacts/arm32-emmc-adma/boot.img)（8-bit 52 MHz HS ADMA2）
- Mali-450 Lima GPU 镜像：[artifacts/arm32-gpu-r2/boot.img](artifacts/arm32-gpu-r2/boot.img)
- ARM64 最小化核心验证镜像（纯 RAM Legacy `uImage`）：[artifacts/arm64-core/uImage](artifacts/arm64-core/uImage)（见 [ARM64-HANDOFF.md](ARM64-HANDOFF.md)）
- ARM64 全功能 + 2 GiB 扩容镜像（纯 RAM Legacy `uImage`）：[artifacts/arm64-all-2g/uImage](artifacts/arm64-all-2g/uImage)（`SHA256: 4196860c3e7f499f286661f929c237d5549cb800313b84ec5306c9209228e85d`）
- ARM64 Linux 7.2.5 全功能 + 2 GiB 扩容镜像（纯 RAM Legacy `uImage`）：[artifacts/arm64-7.2.5-all-2g/uImage](artifacts/arm64-7.2.5-all-2g/uImage)（`SHA256: ba478a9a9fc9a6602a1224fbb759f955d68d7e92c43678b2abbdeb331d1aca09`）

每个产物目录同时保存内核映像（ARM32 的 `zImage` 或 ARM64 的 `Image`）、DTB、initramfs、配置、System.map、vmlinux 和 SHA256 清单。

## 内存和装载地址

| 物理地址 | 用途 |
| --- | --- |
| `0x00000000–0x7fffffff` | U-Boot `bdinfo` 与 LPDDR4 训练报告的完整 2 GiB 物理内存窗口（`arm32-all-2g` 完整放开；`core`/`arm32-all` 等基线镜像仍保留 `0x00000000–0x07ffffff` 128 MiB 窗口）。 |
| `0x053e0000–0x05bdffff`（8192 KiB） | 来自原厂 DT 的日志保留区（`ramoops`/`dmsg_log`/`extend_log`/`bl31_log`/`fastboot_log`/`mntndump`/`pmumem`/`bbox-mem`），标为 `no-map`。 |
| `0x063dc000–0x063dffff`（16 KiB） | 来自原厂 DT 的崩溃缓冲保留区（`kdump`），标为 `no-map`。 |
| `0x08000000–0x083fffff`（4096 KiB） | 原厂 ATF（BL31）保留区，覆盖 `mmc read 0 0x8020000 0x10000 0x1000` 装载的 `0x08020000–0x0821ffff` 及前后安全余量，在 `arm32-all-2g` 中标为 `no-map`。 |
| `0x17ff8000–0x17ffffff`（32 KiB） | U-Boot 启动日志 `Reserve Memory` 低段分配块（`0x17ff8000` 8 KiB、`0x17ffb000` 4 KiB、`0x17ffd000` 8 KiB 及间隔页），在 `arm32-all-2g` 中标为 `no-map`。 |
| `0x7f67f000–0x7fffffff`（9732 KiB） | U-Boot 启动日志 `Reserve Memory` 高段保留区（`Free Addr: 0x7F67F000` 至 `Start Addr: 0x7FFFE000` + 8 KiB，含开机 HDMI/CVBS logo 帧缓冲 `0x7f6c1000+0x7e9000` 等 7 个分配块），在 `arm32-all-2g` 中标为 `no-map`。 |
| `0x01ffbfc0` | Android v1 容器基址，沿用原厂的 `0x4000` 页大小。 |
| `0x01ffffc0`、`0x02000000` | 内层 legacy uImage 头和 ARM zImage 入口。内核自解压并读取附加 DTB，initramfs 内置。 |

U-Boot `bdinfo` 和 DDR 训练日志报告完整 2 GiB（`0x00000000–0x7fffffff`）。`arm32-all-2g` 通过 [hi3798mv320-fiberhome-hg680kd-all-2g.dts](src/hi3798mv320-fiberhome-hg680kd-all-2g.dts) 放开 2 GiB 窗口并以上述 `no-map` 节点避开全部固件与引导保留区（共 22,068 KiB），配合 `CONFIG_HIGHMEM=y`、`CONFIG_BOUNCE=y`（`VMSPLIT_3G` 下 LowMem `0x00000000–0x2fffffff`、HighMem `0x30000000–0x7fffffff`），实机可用 `MemTotal = 2,051,208 KiB`（约 `2003.1 MiB`）。静态工具 [memtest-2g.c](src/memtest-2g.c) 实测分配并校验 `1888 MiB`（`483,328` 页，物理地址覆盖 `0x00000000–0x7f63e000`，其中 `>128 MiB` 460,544 页、`>760 MiB` 320,569 页、`>1536 MiB` 127,033 页）全部通过，且 USB、eMMC ADMA2、GMAC 双向收发及 Mali-450 Lima 离屏渲染均回归通过。串口 `0xf8b00000` 的 75 MHz 时钟与原厂 DT 和 U-Boot 分频寄存器一致。核心验证已成功使用 PSCI 重启并重新拦截原厂倒计时。

## 复现构建

构建目录默认使用 Linux x86-64 主机的 `$HOME/hg680kd-mainline`。已在 `hedc@ArchLinux-E5-Server` 的无特权用户目录验证，使用 36 个并行作业。需要 Bash、GNU make、主机 GCC、bc、bison、flex、cpio、patch、curl、tar、xz、bzip2、gzip 和 flock。封装与传输脚本使用 Python 3，Windows 的 SSH 和串口脚本分别需要 Paramiko 与 pySerial。服务器缺少系统 `bc` 时，本次用 Arch 的 `bc 1.08.2` 软件包把可执行文件解到项目的 `host-tools/bin`。

从本项目同步 `src/` 和 `patches/` 下的文件至构建目录的 `inputs/`，然后在 Linux 主机运行下面的命令。四份上游归档全部按 [source-SHA256SUMS](src/source-SHA256SUMS) 核验。

```bash
root="$HOME/hg680kd-mainline"
mkdir -p "$root/inputs"
cp src/* patches/*.patch "$root/inputs/"
bash scripts/fetch.sh "$root"
bash scripts/build.sh full "$root" "$root/inputs" core
```

`build.sh` 将集成补丁应用到上游源码，复制配套 DTS 和诊断驱动，交叉编译 musl、BusyBox（以及 `gpu`/`all`/`all-fastfetch` 目标中的静态 `lima-test`，`all-fastfetch` 目标额外使用 CMake 3.31.6 静态交叉编译 `fastfetch 2.71.0`）和 Linux，然后把产物装入 `out/release-arm32.tar.gz`。第四个参数可选 `storage`、`emmc`、`usb`、`net`、`sdio`、`gpu`、`all`、`all-fastfetch` 或 `all-2g`，对应产物归档名增加后缀。默认内核工具链为 kernel.org 的 ARM EABI GCC 14.2.0 和 binutils 2.42。构建时间固定为 `2025-12-01 00:00:00 UTC`。本次记录使用服务器 Asia/Shanghai 时区和上述绝对构建目录，BusyBox 构建字符串与内嵌 Kconfig 会记录这些环境数据。

构建脚本按所选目标恢复其他外设补丁，并用 `flock` 保证同一构建根目录只有一个编译任务。新增 `gpu`、`all`、`all-fastfetch` 与 `all-2g` 目标后，使用最终脚本重编 `core`，归档清单仍逐项匹配已上板的核心产物。见 [137-final-core-rebuild-verified.txt](logs/137-final-core-rebuild-verified.txt)、[178-final-core-rebuild-verified-r2.txt](logs/178-final-core-rebuild-verified-r2.txt)、[181-final-core-rebuild-verified-r3.txt](logs/181-final-core-rebuild-verified-r3.txt) 和 [197-core-rebuild-verified-r4.txt](logs/197-core-rebuild-verified-r4.txt)。

Windows 上可用 [remote.ps1](scripts/remote.ps1) 和 [remote.py](scripts/remote.py) 上传输入、执行脚本和下载归档。它们使用已固定的 SSH 主机密钥。密码通过当前 Windows 用户解封 DPAPI 文件，再由标准输入传给 Paramiko。

在 Windows 解包归档后，用原厂 `boot.img` 作为已验证的容器模板。模板的 SHA256 为 `97f4e2b5d88ee6d6fae8ced4809420d7f875c4ff8b0acfe817c6409b6f02a6b8`。该文件位于 U 盘的 `HG680KD` 目录。

```powershell
python .\scripts\pack.py --zimage .\artifacts\arm32-all\zImage `
  --dtb .\artifacts\arm32-all\hi3798mv320-fiberhome-hg680kd-all.dtb `
  --stock-boot J:\HG680KD\boot.img --out .\artifacts\arm32-all
```

## 从 RAM 启动

在原厂 `fastboot#` 提示符下运行。脚本独占 COM4、校验板上 CRC32，之后才执行 `bootm`。串口参数以 115200 8N1 开始和结束，921600 仅用于文件传输。ATF 读取和临时环境变量的命令均沿用原厂启动区布局。

```powershell
python .\scripts\serial_boot.py --baud 921600 `
  --image .\artifacts\arm32-timer-fix\boot.img `
  --before 'mmc read 0 0x8020000 0x10000 0x1000' `
  --boot-command 'bootm 0x1ffbfc0' `
  --shell-command 'cat /sys/devices/system/cpu/online; cat /proc/uptime; sleep 2; cat /proc/uptime' `
  --log .\logs\ram-retest.txt
```

有线已连接时，可将待启动镜像上传到编译服务器的项目目录，并在该服务器用下面的命令发送单个文件。实测传速约 3 MiB/s。例中的板端 IP 仅临时使用，地址需与现场网段和地址分配一致。

```bash
python3 scripts/tftp_send.py out/hg680kd-all.img --bind 192.168.6.160 --peer 192.168.6.249
```

```powershell
python .\scripts\serial_boot.py --image .\artifacts\arm32-all\boot.img `
  --tftp hg680kd-all.img --before 'setenv ipaddr 192.168.6.249' `
  --before 'setenv serverip 192.168.6.160' --before 'setenv tftpdstp 1069' `
  --before 'mmc read 0 0x8020000 0x10000 0x1000' `
  --boot-command 'bootm 0x1ffbfc0' --log .\logs\tftp-retest.txt
```

从 BusyBox 返回 U-Boot 可用 `serial_boot.py --linux --reboot-to-uboot --log logs/reboot.txt`。手动重新上电时，可提前运行 `serial_boot.py --watch-uboot --seconds 180 --log logs/power-cycle.txt` 拦截倒计时。

本项目的验证命令未刷写 eMMC、未执行 `saveenv`。当前启动镜像供 RAM 测试。

## 外设实现和验证边界

### eMMC（`0xf9830000` Nebula SDHCI）

原厂内核 `clk_set_rate_mmc` 与 `g_crg_mask`/`0xc17a3dd8` 表格确认了 Nebula eMMC 的 CRG 频率选择与相位配置：
- `CRG 0x288[26:24]`：`0` = 400 kHz，`1` = 25 MHz，`2` = 48/52 MHz（`MMC_HS`），`3` = 96 MHz，`4` = 144 MHz，`5` = 168 MHz，`6` = 192 MHz。
- `CRG 0x290[19:15]`（驱动相位 `drv_phase`）：`LEGACY` 与 `MMC_HS` 均为 `16 << 15`（`0x00080000`）。
- 控制器 `0x540 |= BIT(4)` 启用软件采样相位，`0x544[7:0]` 在 `LEGACY` 下为 `0x00`，在 `MMC_HS` 下为 `0x04`。
- 原厂 `g_sdhci_nebula_pdata` 仅需 `SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN` 与 `SDHCI_QUIRK2_PRESET_VALUE_BROKEN`，硬件原生支持 8-bit、SDMA 和 32-bit ADMA2。

在保持 `MMC_CAP2_FORCE_READ_ONLY | MMC_CAP2_BOOTPART_NOACC`（`ro=1`）不变的前提下，逐步验证结果如下（两个 64 KiB 固定回归区间：LBA `229376` -> `5628bea3312b887f305f0882461483e3d4663cb412bf742bbda879becc68a08d`，LBA `253280` -> `2edae46864612ccbe27419b4f33b0d147bf0aceefc2001d27c1180d3f5641c3b`；完整 12,304,384 字节原厂 `boot.img` LBA `229376` count `24032` -> `97f4e2b5d88ee6d6fae8ced4809420d7f875c4ff8b0acfe817c6409b6f02a6b8`）：

| 阶段 | 镜像 | 配置 | 固定 64+64 KiB 耗时 / 回归 | 12.3 MB 完整 `boot.img` 回归 | 日志 |
| --- | --- | --- | --- | --- | --- |
| 基线（保留回退） | [arm32-emmc-final](artifacts/arm32-emmc-final/boot.img) / [arm32-storage](artifacts/arm32-storage/boot.img) | 400 kHz, 1-bit, legacy, PIO, `ro=1` | 8.00 s / 一致 | — | [121](logs/121-mainline-emmc-final.txt) / [135](logs/135-mainline-storage.txt) |
| 总线宽度 | [arm32-emmc-w8](artifacts/arm32-emmc-w8/boot.img) | 400 kHz, 8-bit, legacy, PIO, `ro=1` | 1.08 s / 一致 | — | [148](logs/148-mainline-emmc-w8.txt) |
| 时钟 / 高速 | [arm32-emmc-hs](artifacts/arm32-emmc-hs/boot.img) | 52 MHz (48 MHz), 8-bit, `mmc high-speed`, PIO, `ro=1` | 0.08 s / 一致 | 0.58 s / 一致 | [152](logs/152-mainline-emmc-hs.txt) |
| 传输模式 SDMA | [arm32-emmc-sdma](artifacts/arm32-emmc-sdma/boot.img) | 52 MHz (48 MHz), 8-bit, `mmc high-speed`, SDMA, `ro=1` | 0.11 s / 一致 | 0.53 s / 一致 | [156](logs/156-mainline-emmc-sdma.txt) |
| 传输模式 ADMA2 | [arm32-emmc-adma](artifacts/arm32-emmc-adma/boot.img) / [arm32-all](artifacts/arm32-all/boot.img) | 52 MHz (48 MHz), 8-bit, `mmc high-speed`, ADMA2, `ro=1` | 0.11 s / 一致 | 0.55 s / 一致（另测 32 MiB 顺序读无错） | [160](logs/160-mainline-emmc-adma.txt) / [175](logs/175-mainline-all.txt) |

HS200/HS400ES（192 MHz、1.8V 信号电压及 DLL 调谐）与写使能暂未开启。

### 有线网卡（`0xf9840000` GMAC）

网口实测 PHY 地址 1、ID `0x20669908`、MII、100 Mb/s 全双工。通过 U-Boot 实机读取 `0xf9840210`（`MAC_SET`）得到 `0xf0000f2a`，结合原厂内核 `higmac_dev_probe` 与 `higmac_net_xmit` 反汇编确认：`MAC_SET[29:28] == 3` 表示该控制器为支持 TSO/SG 的 `GEMAC_V2`（`higmac-v5`）描述符格式，TX 描述符 `word1` 的 bit 30 为 `DESC_SG`、bit 29 为 `coe_enable`。此前 `r4` 补丁未启用 `HW_CAP_TSO` 且在 `hix5hd2_get_desc_cmd()` 中写入 `DESC_FL_FULL | 0x6bf`（置位了 bit 30 与 bit 29），导致硬件将线性以太网帧误作 Scatter-Gather 描述符解析，线上无有效发送帧。

修正 [0004-NET-hix5hd2-try-Hi3798MV320-GMAC.patch](patches/0004-NET-hix5hd2-try-Hi3798MV320-GMAC.patch) 为 `GEMAC_V2 | HW_CAP_BOOT_CLOCKS | HW_CAP_DESC_16` 并移除错误的 `DESC_FL_FULL | 0x6bf` 分支后：
- 独立镜像 [artifacts/arm32-net-r5/boot.img](artifacts/arm32-net-r5/boot.img) 上板即恢复双向 ICMP 连通（板 -> 服务端/网关 0% 丢包、RTT 0.23–0.38 ms；服务端 -> 板 56 B 与 1472 B MTU-1500 均 0% 丢包、RTT 0.16–0.44 ms，服务端邻居表 `REACHABLE`）。见 [142-mainline-wired-r5.txt](logs/142-mainline-wired-r5.txt)、[143-server-ping-net-r5.txt](logs/143-server-ping-net-r5.txt) 和 [144-net-r5-mtu-and-reboot.txt](logs/144-net-r5-mtu-and-reboot.txt)。
- 合并镜像 [artifacts/arm32-all/boot.img](artifacts/arm32-all/boot.img) 进一步完成 HTTP/TCP 拉取 3,375,104 字节镜像并校验 SHA256 一致（跨 1024 描述符环回绕，0 TX/RX 错误）。见 [175-mainline-all.txt](logs/175-mainline-all.txt)。

### Mali-450 GPU（`0xf9200000` 主线 Lima）

依据本机原厂 DT（`/soc/gpu@f9200000`）、原厂内核反汇编（`hi_gpu_clk_enable`/`hi_gpu_clk_recalc_rate`/`hi_gpu_regulator_get_voltage`）与 U-Boot 实机寄存器抓取（[138-uboot-gmac-gpu-emmc-regs.txt](logs/138-uboot-gmac-gpu-emmc-regs.txt)）：
- 供电寄存器 `0xf8a2301c = 0x009500dd`，按原厂换算公式为 900 mV，U-Boot 已上电，可安全运行于默认 200 MHz；
- 频率选择寄存器 `CRG 0x124 = 0x7` 对应 200 MHz（`7/4/1/6/0/5/2` 分别对应 `200/300/400/460/540/675/712 MHz`）；
- 时钟与复位寄存器 `CRG 0xd4` 在 U-Boot 下为 `0x00000011`（位 4 处于复位有效、位 8/9 时钟关闭）；在 [src/crg-hi3798mv320-reset.c](src/crg-hi3798mv320-reset.c) 中为 `hisilicon,hi3798mv320-crg` 增加 `#clock-cells = <1>` 的 `gpu_bus`（位 0）与 `gpu_core`（位 8/9）门控及频率回读，配合 `resets = <&crg 0xd4 4>` 满足主线 Lima 的先开时钟再解复位顺序；
- Mali-450 内部 L2C0/BCAST/DLBU/PP_BCAST 位于 `0x10000–0x16000`，因此 DT 映射为 `reg = <0xf9200000 0x40000>`，8 路中断（`gp/gpmmu/pp0/ppmmu0/pp1/ppmmu1/pmu/pp`）全部绑定到 GIC SPI 94（hwirq 126，电平高有效）。

主线 Lima 驱动零改动即完成绑定，识别 `gp` + `pp0` + `pp1`（`gp_ver=0x0d070000`，`pp_ver=0xcf070000`）、`l2_cache0`（8 KiB）与 `l2_cache1`（64 KiB），注册 `/dev/dri/renderD128` 与 `/dev/dri/card0`。静态测试程序 [src/lima-test.c](src/lima-test.c) 通过 `DRM_IOCTL_LIMA_GEM_SUBMIT` / `DRM_IOCTL_LIMA_GEM_WAIT` 连续提交两次完整 GP（PLBU 建块）+ DLBU + 双 PP 64×64 RGBA8888 离屏清屏写回任务，两次读回 4096/4096 像素全部精确匹配，共享中断计数递增且内核日志零错误。DVFS 调频调压和 HDMI 显示输出留待后续适配。见 [170-mainline-gpu-lima-gp-pp.txt](logs/170-mainline-gpu-lima-gp-pp.txt) 和 [175-mainline-all.txt](logs/175-mainline-all.txt)。

### USB 与板载无线

USB U 盘挂在 `0xf98a0000` 控制器，由 [src/xhci-hi3798mv320.c](src/xhci-hi3798mv320.c) 初始化两路 USB2 PHY 与 OTP trim 后交由通用 xHCI 接管，480 Mb/s U 盘只读挂载与完整原厂备份 SHA256 在独立镜像与 `arm32-all` 合并镜像中均通过。SuperSpeed 路径和 `0xf9890000` OTG 控制器仍待验证。

原厂通用 DT 启用了 `0xf9c40000` SDIO 主机。ARM32 历史试验加上 GPIO4_3 仍无法收到 CMD5 响应。ARM64 按原厂补齐时钟和相位后复测结果一致，现场绑带与原厂启动脚本将本机识别为单蓝牙版本。[ARM32 命令追踪](logs/125-sdio-power-command-trace.txt) · [原厂板型与 ARM64 实测](WIFI-REVERSE.md)

原厂引导链的 ARM64（AArch64）纯 RAM 交接与全套核心外设（USB、eMMC ADMA2、GMAC、Mali-450 Lima）+ 2 GiB 内存扩容均已实机验证通过（见 [ARM64-HANDOFF.md](ARM64-HANDOFF.md) 和 [218-mainline-arm64-all-2g-full.txt](logs/218-mainline-arm64-all-2g-full.txt)）。既有 `arm32` `core` 目标可复现性已同步复验逐项一致（见 [212-core-rebuild-verified-arm64.txt](logs/212-core-rebuild-verified-arm64.txt) 和 [221-core-rebuild-sha256.txt](logs/221-core-rebuild-sha256.txt)）。音频、视频解码与 GPU DVFS 留待后续适配。[海思 Hi3798MV320](https://www.hisilicon.com/cn/products/smart-media/STB/Hi3798MV320) · [Hi3798MV300 适配记录](https://github.com/hataketsu/hi3798mv300-mainline)
