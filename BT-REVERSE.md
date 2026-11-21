# HG680KD 板载蓝牙逆向与 ARM64 适配

实机为 `lsadcix=11`、`boardid=4` 的烽火 HG680KD，硬件版本 `9900E00100062401ND0XX1L1`。独立芯片为 **Realtek RTL8761BTV-VAT**，经 Hi3798MV320 的 UART2 运行 HCI H5。Linux 6.18.64 自动绑定 `hci0`，加载原厂固件 `0x1ed6b420`，并收到周边设备的 BLE 广播。[板型逆向](WIFI-REVERSE.md) · [最终串口](logs/305-mainline-arm64-hdmi-bt-final.txt)

## 原厂只读证据

- eMMC 整盘和两个分区的 `ro` 均为 1。`system` 位于 `0x0d400000`（1200 MiB），`vendor` 位于 `0x8a400000`（400 MiB）。两者分别用 ext4 `ro,noload` 挂载到 RAM 文件系统的 `/tmp`。原厂布局无独立 ODM 分区，system 根目录下 `/odm` 只有到 `/vendor/odm/*` 的链接，本机 vendor 无 `odm` 目录。[实机挂载和目录](logs/277-bt-stock-partitions.txt) · [板上 SHA256](logs/284-bt-stock-hashes-reboot.txt)
- `init.fhservice.sh` 在板型 4 选择 `ro.fh.ddr4.btonly`。`wifi_bt_init.sh` 的相应分支按原厂有线 MAC 加 1 设置 `ro.boot.btmacaddr`。通用的 `init.wifibt.rc` 会加载 `rtk_btusb.ko`，选择器 `socwifi.cfg` 的 RTL8761BTV 条目则是 `bt_only`、零个 VID/PID，指定 `libbt-vendor-rtk.so`。原厂 USB 树仅发现外接 SanDisk U 盘，实机 HCI 枚举路径位于 UART2。[原厂选择器](logs/270-wifi-stock-btonly-selection.txt) · [原厂 USB 树](logs/261-wifi-uboot-registers.txt)
- `android.hardware.bluetooth@1.0-service.rc` 启动 HIDL HAL。HAL 实现从 `bluetooth.vendor.lib` 选择库，回退名称是 `libbt-vendor.so`。通用库包含 `bt_vendor.conf` 和 `/dev/ttyO1` 字符串，本机 vendor 的 `/etc/bluetooth` 实际提供 `rtkbt.conf`，其中一行为 `BtDeviceNode=?/dev/ttyAMA1:H5`。system 对应目录提供 `bt_stack.conf` 和 `bt_did.conf`。Realtek 库的 build 字符串为 `20220826_BT_ANDROID_9.0_8761BTV_VAT`，UART 固件表在文件偏移 `0x28170` 的 `0x8761` 记录指向 `rtl8761btv_vat_fw` 与 `rtl8761btv_vat_config`。[HAL、库存及配置](logs/278-bt-stock-binary-inventory.txt) · [固件表](logs/290-bt-rtk-patch-table.txt)

## UART、复位与唤醒

| 项目 | 结论与依据 |
| --- | --- |
| 控制器和中断 | 原厂 DT 将 `uart2` 指向 `/soc/amba/uart@f8b02000`。该 ARM PL011 rev2 的 MMIO 为 `0xf8b02000–0xf8b02fff`，GIC SPI 51（hwirq 83，高电平触发），UART 时钟为 75 MHz。原厂 PL011 查找的是 `serial` alias，原厂 DT 用 `uart` alias，按启用节点顺序分配 `ttyAMA0`、`ttyAMA1`、`ttyAMA2`。因此 `ttyAMA1` 对应 UART2，而实机 `hci0` 的 sysfs 路径也经过 `f8b02000.serial`。 |
| UART 参数 | H5 初始为 115200、8E1、关闭硬件流控。原厂 VAT 配置的 `0x000c` 条目长 16 字节，起始 LE32 为 `0x04928002`，从零开始计数的字节 12 为 `0x5f`，bit 2 = 1。主线先发送 `0xfc17`，再切换到 1500000 baud、8E1 并启用 RTS/CTS，随后下载固件。 |
| 引脚复用 | 原厂 UART2 节点依赖引导链保留的 IOCFG，未提供 pad 配置。已读回 IOCFG 起始窗口，实机链路在保留其状态时工作。DTS 声明 `uart-has-rtscts`。TX、RX、RTS、CTS 对应的 IOCFG 位和封装引脚仍未逐根定位。 |
| `BT_REG_ON` | 原厂 `rfkill_set_power` 请求 GPIO34，即 PL061 `0xf8b24000` 的 GPIO4_2。unblock 驱动高，block 驱动低，每次切换后延时 10 ms。原厂 U-Boot 的 GPIO4 掩码数据读回 `0x04`，方向读回 `0x0c`。主线 H5 以该高有效线完成关断复位，驱动低 100 ms、拉高后等待 500 ms，再进行 H5 握手。运行中 debugfs 读回该线为输出高。 |
| `BT_RST`、`BT_WAKE`、32.768 kHz | 实机初始化只需切换 GPIO4_2，GPIO4_3 在原厂通用 Wi-Fi 代码中单独控制。Realtek 库访问 `/proc/bluetooth/sleep/lpm`，原厂 DT 和已识别的 rfkill 路径未提供独立复位或 wake GPIO 的映射。运行通过的板级 DTS 无需配置额外的 32.768 kHz 时钟。独立连线是否存在、外部低速时钟来源及挂起唤醒尚未取得原理图或实测证据。 |

[原厂 UART 节点与主线 H5](logs/280-bt-uart-upstream-analysis.txt) · [原厂 PL011 与 rfkill 反汇编](logs/285-bt-stock-kernel-disassembly.txt) · [U-Boot 寄存器](logs/286-bt-uboot-registers.txt) · [原厂时钟和 wake 路径](logs/303-bt-clock-and-wake-analysis.txt) · [实机 UART 参数及重绑定](logs/308-bt-h5-runtime-settings.txt) · [Linux H5 驱动](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_h5.c)

## 主线适配和固件

第一次 RAM 启动读到 `hci_ver=06 hci_rev=000a lmp_ver=06 lmp_subver=8761`。Linux 已有 `0a/000b/8761` 的通用 RTL8761B UART 表项，VAT 的这个 ROM 组合没有命中，其通用恢复路径让 H5 对端重启。新补丁仅为实测 UART 组合添加 ROM 匹配，使用 legacy epatch，跳过 VAT 无需的 security-project 读取。固件扩展区的 project ID 是 3，ROM version 是 3，下载时选择索引 4，实际发送 21,738 字节（含 110 字节配置）。ROM 回报和原厂固件表共同确定型号。[首次 RAM 探测](logs/292-bt-first-ram-boot.txt) · [固件格式与配置字节](logs/280-bt-uart-upstream-analysis.txt) · [btrtl 补丁](patches/0007-Bluetooth-btrtl-support-RTL8761BTV-VAT-ROM.patch)

新目标基于 HDMI DTS，启用 PL011、PL061 和 Realtek H5 serdev。蓝牙核心、BR/EDR、LE、HCI UART、H5、Realtek 协议及固件加载器均内建。DTS 沿用 Wi-Fi 探测镜像的 USB 默认 PHY trim 配置，eMMC 保留内核强制只读和只读分区映射。镜像内的 `bt-test` 使用 HCI 用户通道读取版本及 BD_ADDR，可限时被动扫描并打印实际广播数据。[DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi-bt.dts) · [内核配置](src/arm64-all-2g-hdmi-bt.config) · [serdev 绑定](patches/0006-Bluetooth-Realtek-RTL8761BTV-serdev.patch) · [验证工具](src/bt-test.c)

| 原厂 vendor 路径 | initramfs 中的路径 | SHA256 |
| --- | --- | --- |
| `/firmware/rtl8761btv_vat_fw` | `/lib/firmware/rtl_bt/rtl8761btv_vat_fw.bin` | `846ee9c3b2b4e45f9317d7307753374bd1aa401de9586460684b6b6b543047fe` |
| `/firmware/rtl8761btv_vat_config` | `/lib/firmware/rtl_bt/rtl8761btv_vat_config.bin` | `7309d5f317fb80f630c6dca08c09559cf2db834801b86ded9ff0b22416a5e07c` |

两份原厂文件来自已与实机逐文件核对过的只读 vendor 镜像。构建前将原始文件名保留在服务器 inputs 目录，构建脚本验证哈希后将其放入 initramfs。[板上 SHA256](logs/284-bt-stock-hashes-reboot.txt) · [构建输入清单](src/BT-FIRMWARE-SHA256SUMS)

## 复现与验证

将 `src/` 和 `patches/` 中的构建输入同步到服务器的 `inputs/`，并同步 `scripts/build.sh` 和 `scripts/build_arm64.sh`。已有分区副本可直接通过 `debugfs` 的默认只读模式提取。

```bash
root="$HOME/hg680kd-mainline"
vendor="$root/analysis/hdmi/blocks/vendor.img"
for name in rtl8761btv_vat_fw rtl8761btv_vat_config; do
    debugfs -R "dump /firmware/$name $root/inputs/$name" "$vendor"
done
(cd "$root/inputs" && sha256sum -c BT-FIRMWARE-SHA256SUMS)
bash scripts/build.sh full "$root" "$root/inputs" arm64-all-2g-hdmi-bt
```

取回并解包 `out/release-arm64-all-2g-hdmi-bt.tar.gz`，使用 `pack_arm64.py` 封装 RAM 镜像。

```powershell
$out = 'artifacts/arm64-all-2g-hdmi-bt'
python scripts/pack_arm64.py --image "$out/Image" `
  --dtb "$out/hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi-bt.dtb" `
  --out $out --name boot.img
```

镜像 `artifacts/arm64-all-2g-hdmi-bt/boot.img` 的 SHA256 为 `73d68394f7c1b20c7d031db27773224c87b3a406b09c10220d188f3ab5c3ccc8`。TFTP 到 `0x01fdffc0` 后，板上 CRC32 `451a6a2c` 与本地一致，执行 `bootm 0x1fdffc0`。COM4 使用 115200 8N1。最终运行中 eMMC 整盘及两个分区全程 `ro=1`，原厂 boot 容器 12,304,384 字节的 SHA256 为 `97f4e2b5d88ee6d6fae8ced4809420d7f875c4ff8b0acfe817c6409b6f02a6b8`。RAM 验证未刷写 eMMC，未操作 OTP，未执行 `saveenv`。[镜像 SHA256 清单](artifacts/arm64-all-2g-hdmi-bt/BOOT-SHA256SUMS) · [构建日志](logs/300-bt-final-build.txt) · [最终串口](logs/305-mainline-arm64-hdmi-bt-final.txt)

| 验证项 | 实机结果 |
| --- | --- |
| 蓝牙 HCI | `hci0` 自动出现，原厂固件和配置的运行时哈希逐项匹配。ROM version 3，固件 `0x1ed6b420`，HCI `09/1ed6`，LMP `09/b420`，厂商 `005d`。控制器返回 `00:E0:4C:23:99:87`，RAM 测试保留该地址，未应用 Android 的有线 MAC 派生规则。 |
| BLE 被动扫描 | `bt-test 10` 在 10 秒内收到 4 条广播报告，带广播数据和 RSSI，返回 0。再次执行 `bt-test` 读取版本及地址也返回 0。 |
| HDMI 和 Lima | `/dev/dri/card0` 为 simpledrm，`/dev/fb0` 为 1920×1080，pitch 7680，彩条帧缓冲逐行读回。HDMI 采集的 24 个像素样本全部通过。Lima 使用 `/dev/dri/renderD128`，两次 GP+PP 各匹配 4096/4096 像素。[采集帧](artifacts/arm64-all-2g-hdmi-bt/hdmi-color-bars.png) · [采集校验](logs/307-bt-final-hdmi-capture.txt) |
| CPU 和内存 | 4 核在线，1840 MiB 分配及校验通过。 |
| GMAC 和 USB | 100 Mb/s 全双工，编译服务器 ICMP 3/3，SanDisk `0781:5591` 以 480 Mb/s 枚举，USB FAT 按只读方式挂载。 |
| eMMC | 8-bit high-speed ADMA2，52 MHz 请求、48 MHz 实际，`ro=1` 及原厂 boot 容器 SHA256 均通过。 |

Linux 7.2.5 前向移植目标 `arm64-7.2.5-all-2g-hdmi-bt` 已通过同口径全项实机回归（[artifacts/arm64-7.2.5-all-2g-hdmi-bt/boot.img](artifacts/arm64-7.2.5-all-2g-hdmi-bt/boot.img)，SHA256 `8d77c2a170fb75bdd38db2991f380bdf5755185e15fc30538221711b603eb695`）：`hci0` 加载 `0x1ed6b420` 固件并读出 `00:E0:4C:23:99:87`、`bt-test 10` 收到 4 条真实 BLE 广播，HDMI 采集 24/24 样本通过，Lima 两次 4096/4096 像素匹配，4 核 SMP、1840 MiB 内存校验、GMAC ICMP 3/3、USB 480 Mb/s 只读挂载与 eMMC `ro=1` 全部通过。详见 [构建日志](logs/309-arm64-7.2.5-hdmi-bt-build.txt) · [最终串口](logs/312-mainline-arm64-7.2.5-hdmi-bt-final.txt) · [HDMI 采集校验](logs/313-arm64-7.2.5-hdmi-capture.txt)。

回退镜像 [arm64-all-2g-hdmi-wifi-probe/boot.img](artifacts/arm64-all-2g-hdmi-wifi-probe/boot.img) 的 SHA256 仍为 `f57038eb565ef04366282a7b0a6ed2ac53fd3258df5361684a1a4cf5b4d8276d`。
