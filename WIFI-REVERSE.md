# HG680KD 无线板型与 SDIO 探测

## 本机是原厂单蓝牙版本

同一台盒子的原厂 `deviceinfo` 给出 `FIBERHOME_HG680KD`、`HG680-ND8XX-1N1`，U-Boot 参数为 `androidboot.hardware.lsadcix=11` 与 `androidboot.hardware.boardid=4`。原厂 `/vendor/bin/init_hardware_info` 从 `ro.fh.lsadcix` 查表。ARM Thumb 函数 `0x1918` 对索引 11 依次读取 GPIO19、20、21，以 bit 0、1、2 组成 `ro.fh.boardid`。U-Boot 当场读 GPIO2 掩码窗口 `0xf8b220e0 = 0x20`、方向寄存器 `0xf8b22400 = 0x40`，三根输入为低、低、高，独立复核板型 4。[原厂引导参数](logs/01-uboot-probe.txt) · [绑带与 RAM 引导](logs/267-mainline-arm64-hdmi-wifi-final.txt) · [查表及反汇编](logs/274-wifi-stock-kernel-and-board-disassembly.txt)

原厂 `/system/bin/opt/etc/init.fhservice.sh` 对板型 1、2、4 取 `ro.fh.ddr4.btonly`，写入 `ro.product.hardware.version`。同一台盒子的 `/vendor/default_stb.prop` 将该项标为「单蓝牙硬件版本号」，值为 `9900E00100062401ND0XX1L1`。Wi-Fi 5、Wi-Fi 6 是不同的原厂硬件版本。原厂 `wifi_bt_init.sh` 在板型 4 的分支只派生蓝牙 MAC，在板型 0 的分支才加载 `hi_sdio_detect.ko` 和展锐 BSP。文件来自实机只读挂载，SHA256 与服务器副本一致。实机未发现 Wi-Fi 枚举设备，PCB 贴装情况未拆机复核。[原厂板型脚本和属性](logs/270-wifi-stock-btonly-selection.txt) · [实机读取脚本](logs/271-wifi-live-stock-sku.txt) · [实机读取属性](logs/272-wifi-live-stock-properties.txt)

## 原厂数据及驱动选择

ARM64 探测镜像使用 `blkdevparts` 暴露两个只读分区。原厂 `system`（起点 `0x0d400000`、1200 MiB）和 `vendor`（起点 `0x8a400000`、400 MiB）分别映射为 `mmcblk0p1`、`mmcblk0p2`。整盘及两个分区的 `ro` 均为 1，均使用 ext4 `ro,noload` 挂载。原厂分区表没有独立 ODM 分区。system 根目录的 `/odm` 内容是指向 `/vendor/odm/*` 的符号链接，本机 vendor 没有该目录。另从整盘以 `O_RDONLY` 提取 `deviceinfo` 和 `dtbo` 到构建服务器。DTBO 容器只有一个 FDT，SHA256 为 `e289000306072f9fd2e140dc5a837fb646b9c803dae15c2b4d0b99c32cf63517`，该 FDT 是完整原厂设备树。[只读分区副本](logs/237-hdmi-readonly-partition-analysis.txt) · [只读传输与容器](logs/257-wifi-readonly-transfer.txt) · [原厂 DT](logs/262-wifi-factory-dtbo-topology.txt) · [实际挂载](logs/267-mainline-arm64-hdmi-wifi-final.txt)

`/vendor/etc/init/init.wifibt.rc` 拉起 `/vendor/bin/wifi_bt`，后者读取 `/vendor/etc/wifi/socwifi.cfg` 并检查 USB、SDIO、PCI 三条 sysfs 总线。原厂库存和模块声明适用于多个板型。下表的 ID 是原厂匹配条目，本机没有读到这些 ID。[文件及模块别名](logs/253-wifi-factory-modules.txt) · [加载器和启动脚本](logs/254-wifi-factory-loader.txt) · [匹配配置和固件路径](logs/255-wifi-factory-selection.txt)

| 原厂候选 | 总线和匹配 ID | 原厂驱动与固件证据 |
| --- | --- | --- |
| Realtek RTL8822CS | SDIO `024c:c822`。模块另声明 `024c:d821` | `rtl8822cs.ko` 内含 Wi-Fi 固件数组 `array_mp_8822c_fw_nic`。目录中的 `rtl8822cs_fw` 与 `rtl8822cs_config` 是 Realtek 蓝牙固件和配置命名，主线 rtw88 的 Wi-Fi 请求名为 `rtw88/rtw8822c_fw.bin`。 |
| 展锐 UWE5621DS | SDIO，原厂选择器接受 `0000:0000` | `uwe5621_bsp_sdio.ko` 查找 `wcnmodem.bin`，Wi-Fi 层由 `uwe5621_wifi_sdio.ko` 提供。vendor 也包含 `wifi_56630001_3ant.ini`。 |
| MediaTek MT7920EN | PCIe `14c3:7961` | `wlan_mt7961_pcie.ko`，vendor 提供 `WIFI_RAM_CODE_MT7961_1a.bin` 与 patch、EEPROM 文件。 |
| Realtek RTL8761BTV | 原厂 `bt_only`，无 VID/PID 匹配字段 | `libbt-vendor-rtk.so`。该条仅提供原厂蓝牙选型线索。 |

## SDIO、USB 与 PCIe 条件

- **供电和复位。** `hi_sdio_detect.ko` 的 GPIO 默认值为 35，即 GPIO4_3（PL061 `0xf8b24000`）。`init_module` 先拉低，helper 内部等约 100 ms，调用方再等约 100 ms。请求拉高时 helper 先维持低电平再等约 100 ms，随后拉高并由调用方等约 100 ms，最后执行 `sdhci_nebula_sdio_rescan(1)`。合计低约 300 ms，拉高后约 100 ms 才开始 rescan。原厂内核的 `wifibt_rfkill_power_init` 也对 GPIO35 做低、高切换，helper 每次约 10 ms。GPIO34（GPIO4_2）在原厂 rfkill 中独立命名为 `BT_REG_ON`，高有效。U-Boot 下 GPIO4_2、GPIO4_3 均为输出高。探测 DTS 保持 GPIO4_2 原状态，GPIO4_3 只由 SDIO pwrseq 控制。[模块反汇编](logs/253-wifi-factory-modules.txt) · [原厂 GPIO 与主机反汇编](logs/274-wifi-stock-kernel-and-board-disassembly.txt) · [U-Boot GPIO 电平](logs/261-wifi-uboot-registers.txt)
- **引脚、时钟与复位。** 原厂 SDIO 主机为 `0xf9c40000`、SPI 86、`devid=1`、4-bit。原厂驱动的 SDIO IOCFG 表列出 `0xf8a21000` 加偏移 `0x90–0xa4` 的六个 pad，U-Boot 现场值依次为 `0x1071, 0x1071, 0x2001, 0x1071, 0x1071, 0x1071`，低位均为 1。探测镜像继承原厂 mux。CRG `0x2a4` 的 bit 0/1 门控时钟，bit 16/17/18 复位主机及 TX/RX，bits 24–26 选择频率。DLL 复位为 `0x2a8` bit 1。原厂 legacy 驱动相位是 `0x2ac` 的 `0x80000`（掩码 `0xf8000`），主机 `0x540` bit 4 选择软件采样，`0x544[7:0]` 设为 0。探测镜像按该设置以 400 kHz、3.3 V 发送初始化命令。[原厂 DT](logs/262-wifi-factory-dtbo-topology.txt) · [原厂常量与反汇编](logs/274-wifi-stock-kernel-and-board-disassembly.txt) · [U-Boot 寄存器](logs/261-wifi-uboot-registers.txt)
- **低速参考时钟及电源域。** 原厂 DT 的 SDIO 节点和 GPIO35 的探测模块均未声明独立 `ext_clock`、`vmmc`、`vqmmc` 或 WLAN LDO 控制。UWE BSP 有独立的芯片使能和复位流程，`chip_power_on` 在芯片使能后等 20 ms，再释放复位并进入总线扫描流程。实机没有相应模组的正向证据，未测 PCB 上是否布有外部 32.768 kHz 时钟或独立 LDO。[原厂 DT](logs/262-wifi-factory-dtbo-topology.txt) · [UWE BSP 反汇编](logs/268-wifi-module-power-disassembly.txt)
- **USB 与 PCIe。** U-Boot 的 `usb start` 扫描两路控制器，外设只有 SanDisk 存储。主线 xHCI 同样仅枚举 `0781:5591`，速度 480 Mb/s。原厂 `hiclk_prepare_pcie` 根据 PERI `0x870` bit 0 进入 `PCIe hard disable` 分支，本机 `0xf8a20870=0x00f00005`，历史原厂串口也记录了该分支。[U-Boot USB 树](logs/261-wifi-uboot-registers.txt) · [PCIe 反汇编](logs/274-wifi-stock-kernel-and-board-disassembly.txt) · [历史原厂串口](logs/29-mainline-usb-storage.txt) · [本次寄存器与 USB 回归](logs/267-mainline-arm64-hdmi-wifi-final.txt)

## ARM64 RAM 验证

探测镜像在 eMMC 注册为 `ro=1` 后从 `mmc1` 初始化 SDIO。CMD0 没有响应阶段，主机成功发送 CMD0 只能证明它完成了本地命令流程。CMD52 和 CMD5 均返回 `-110`，CMD5 的四次尝试响应全零，SDIO 设备目录为空，网络接口只有 `eth0` 和 `lo`。厂商选型明确为单蓝牙，加上无枚举结果，当前硬件没有可绑定的 WLAN 设备，探测镜像没有集成芯片专用 WLAN 驱动或固件，热点扫描未执行。[ARM64 完整串口](logs/267-mainline-arm64-hdmi-wifi-final.txt)

镜像由原厂 U-Boot 加载到 RAM，板上 CRC32 为 `0c5f261f`。探测 DTS 省去 USB 的 OTP trim 资源，USB PHY 沿用默认 trim。运行期间 eMMC 原厂 boot 容器的全量 SHA256 与回退基线一致。HDMI 注册 `card0` 和 `fb0`，采集卡收到 1080p 八色彩条，24 个采样点全部匹配。Lima `renderD128` 的两次渲染均匹配 4096/4096 像素。四核在线，1840 MiB 内存读写通过，GMAC 100 Mb/s 全双工 ping 0% 丢包，USB FAT 按只读挂载，`fastfetch` 正常。[SHA256 清单](artifacts/arm64-all-2g-hdmi-wifi-probe/BOOT-SHA256SUMS) · [串口回归](logs/267-mainline-arm64-hdmi-wifi-final.txt) · [采集卡截图](artifacts/arm64-all-2g-hdmi-wifi-probe/hdmi-color-bars.png) · [采集像素校验](logs/273-wifi-image-hdmi-capture.txt)
