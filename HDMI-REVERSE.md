# 烽火 HG680KD（Hi3798MV320）HDMI 寄存器与 DRM 适配记录

板级证据来自同一台 HG680KD 的原厂 U-Boot、原厂 ARM32 内核及 DTS。原厂 `fastboot.bin` 的链接基址为 `0x00c00000`，SHA256 为 `c0889ada28f6b8ccab76b983394a0470028efd8324412eb8ce102c64480a8265`。从原厂 `boot.img` 解出的内核映像 SHA256 为 `a47592ad5a842035f0bd6e41fdce71cb89c2238aff57e2b333d60213af176724`，下列内核函数地址使用恢复的 kallsyms 虚拟地址。原厂 boot 容器的 SHA256 为 `97f4e2b5d88ee6d6fae8ced4809420d7f875c4ff8b0acfe817c6409b6f02a6b8`，与主线基线从 eMMC 完整读出的哈希一致。[ARM64 引导链](ARM64-HANDOFF.md) · [原厂 DT 摘要](logs/04-factory-dtb-summary.txt) · [基线只读回归](logs/218-mainline-arm64-all-2g-full.txt)

## 1. Logo 显存与 HDMI 时序

U-Boot 的 JPEG 日志报告输入、解码输出均为 **1280×720**。`0x00c61b20`–`0x00c61c08` 可循 Logo 入口追到 `ShowGraphicFirstFrameLogo`（`0x00c6ec54`），后者调用图形层显示路径。`0x00c6f470`–`0x00c6f4ac` 计算按 16 字节对齐的 32 bpp 行宽，并以行宽乘高度申请 `GRAPHIC_GFBG_HD` 保留内存。`0x00c886d4` 构造 VDP `0xf8cc0000` 基址。Logo 亮屏时，在 `fastboot#` 用 `md.l` 读到了下面的值。[原厂启动与保留区](logs/225-hdmi-reboot-to-uboot.txt) · [现场寄存器](logs/226-hdmi-uboot-registers.txt) · [接采集卡后冷启动复核](logs/240-hdmi-uboot-capture-card.txt)

| 物理地址 | 实测值 | 解释与反汇编依据 |
| --- | --- | --- |
| `0xf8cc8a00` | `0x00001001` | GFX0 FDR 工作配置，GFX1 的对应值为 `0x00001000`，图形层间隔 `0x200`。 |
| `0xf8cc8a40` / `0xf8cc8b2c` | `0x00000068` | GFX0 配置和活动格式。`vdp_fdr_gfx_set_ifmt`（`0xc05fbbd0`）更新低 8 位，`DRV_GFBG_HAL_GFX_GetInDataFmt`（`0xc066ceb8`）读取同一字段。`0x68` 对应 ARGB8888，内存中每像素按 B、G、R、A 排列。 |
| `0xf8cc8a44` / `0xf8cc8b20` | `0x0437077f` | 源和活动尺寸高 16 位为 `height−1`，低 16 位为 `width−1`。`vdp_fdr_gfx_set_src_h/w`（`0xc05fbc3c` / `0xc05fbca8`）确认字段，解得 **1920×1080**。 |
| `0xf8cc8a48` / `0xf8cc8a4c` | `0` / `0x0437077f` | 源原点和图形矩形末端。 |
| `0xf8cc8a50` / `0xf8cc8a54` | `0` / `0x7f6c1000` | 图形面配置地址的高、低 32 位。`vdp_fdr_gfx_set_gfx_addr_h/l`（`0xc05fbec4` / `0xc05fbf28`）。 |
| `0xf8cc8b38` | `0x7f6c1000` | **实际扫描地址**。`DRV_GFBG_HAL_GFX_GetWorkLayerAddr`（`0xc066c15c`）读取 `VDP + 0x8b38 + layer * 0x200`。 |
| `0xf8cc8a60` | `0x1e0` | `DRV_GFBG_HAL_GFX_GetLayerStride`（`0xc066cb1c`）将低 16 位左移 4，得 **7680 字节/行**。`7680 * 1080 = 0x7e9000`，和 U-Boot 的分配大小相同。 |
| `0x7f6c1000` | `0xff230833`（起始四像素） | 可读的未压缩像素。`0x7feab000` 的 `0x152000` 分配是另一块内存，尺寸与对齐后的 720p YUV 解码缓冲相符。 |
| `0xf8ccc200` | `0x80008010` | 活动显示通道的控制值。相邻的 `0xf8ccc000` 为 `0`，不能套用 CV200 的通道选择。 |
| `0xf8ccc208` / `0xf8ccc210` | `0x0051086f` / `0x017f0eff` | 活动区计数的低位分别为 `2159` 和 `3839`，与下方 AVI 声明的 2160 行、每行 3840 像素一致。 |
| `0xf8ce1818` / `0xf8ce181c` / `0xf8ce1820` | `0x000d0282` / `0x00a870f6` / `0x00000061` | AVI InfoFrame 头及前两段负载。`hdmi_reg_avi_pkt_header_hb_get`（`0xc084afb8`）和 `hdmi_reg_{l,h}_avi_pkt0_pb_get`（`0xc084b018` / `0xc084b080`）确认字节位置。校验和成立，DB1 的 Y 字段为 3（YCbCr 4:2:0），DB4 的 VIC 为 `0x61`（97，**3840×2160p60**）。 |

本次实机有三个尺寸。JPEG 解码为 720p，GFX0 显存为 1080p，U-Boot 发出的 AVI InfoFrame 声明 2160p60。采集卡的 1920×1080 是它送给 Windows 的 USB 视频尺寸。simpledrm 的固定 KMS 模式应使用 GFX0 尺寸和 pitch。[Linux CTA VIC 表](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/drm_edid.c) · [Hi3798CV200 同系列格式交叉参考](https://github.com/olragon/himedia-q10-pro-linux/blob/main/docs/261011-himedia-hdmi-simplefb-research.md)

实验 DTS 把原有高端固件保留区拆成三个连续、无重叠的 `no-map` 区间。显存区是 `[0x7f6c1000, 0x7feaa000)`，前后两段继续覆盖原有保留区。`/chosen/framebuffer@7f6c1000` 用 `reg` 提供物理窗口，Linux 6.18 的 simpledrm 对它使用 `devm_ioremap_wc()`。`console=tty0` 启用屏幕控制台，末尾的 `console=ttyAMA0,115200n8` 保持 `/dev/console` 上的串口 shell。`clk_ignore_unused` 保持固件已开启的时钟。显存以外的 VDP、时钟和 HDMI 状态均由原厂 U-Boot 建立。[DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi.dts) · [Linux simple-framebuffer binding](https://github.com/torvalds/linux/blob/v6.18/Documentation/devicetree/bindings/display/simple-framebuffer.yaml)

最终镜像通过 U-Boot CRC32 `5b0e98bf` 校验。`fb-test` 以 `MAP_SHARED` 映射 fbdev，写满八色彩条并 `fsync()` 触发刷新，随后回读 1080 行。早期试验逐行调用 `pwrite()`，6.18.64 的 `drm_fb_helper_memory_range_to_clip()` 对恰好写到单行末尾的更新计算出零宽剪裁，因此只更新了 fbdev 缓冲区。最终镜像的采集帧在每条色带的三个高度共取 24 点，按高低 RGB 分量全部匹配。测试先降低屏幕日志级别并隐藏光标，避免控制台重绘覆盖色带。[最终串口](logs/249-mainline-arm64-all-2g-hdmi-final.txt) · [采集校验](logs/250-hdmi-capture-card-validation.txt) · [彩条 PNG](artifacts/arm64-all-2g-hdmi/hdmi-color-bars.png)

## 2. 物理窗口与 IRQ

| 模块 | 物理地址 | 原厂二进制证据 |
| --- | --- | --- |
| VDP | `0xf8cc0000` | `disp_init`（`0xc0642b20`–`0xc0642b28`）将其传入 `disp_hal_init`。 |
| HDMI TX 控制 | `0xf8ce0000` | `hdmi_tx_ctrl_regs_init`（`0xc08460a0`）映射 `0x134` 字节。 |
| HDMI TX 视频 / 音频 | `0xf8ce0800` / `0xf8ce1000` | `hdmi_video_path_regs_init` / `hdmi_audio_path_regs_init` 分别映射 `0x1ac` / `0x54` 字节。 |
| HDMI TX 数据包 | `0xf8ce1800` | `hdmi_tx_hdmi_regs_init` 映射 `0x284` 字节，AVI 位于这片窗口。 |
| HDMI TX AON / DDC | `0xf8ce4000` + TX 控制窗口 | `hdmi_tx_aon_regs_init` 映射 `0x64` 字节。DDC 并入 TX，详见下节。 |
| HDMI TX CEC | `0xf8ce4800` | `hdmi_tx_cec_regs_init` 映射 `0x68` 字节。 |
| HDMI TX HDCP / MCU | `0xf8ce2000` / `0xf8cfc000` | `hdmi_tx_hdcp_regs_init` / `hdmi_mcu_cpu_regs_init` 映射 `0xfc` / `0x3c0` 字节。 |
| HDMI PHY | `0xf8d30000` | `hdmi_phy_regs_init`（`0xc086bd9c`）映射 `0xc0` 字节。 |
| CRG / I/O 配置 | `0xf8a22000` / `0xf8000048`、`0xf8000210` | `hdmi_crg_regs_init` 映射 CRG `0x4c4` 字节。后两段用于部分芯片分支的 HDMI 时钟/引脚适配。 |

原厂 DT `/soc/virtdev` 按同一索引排列 `interrupt-names` 与 `interrupts`，全部为 GIC 高电平触发。`hdmi` 为 SPI **88**（INTID 120），`vdp` 为 SPI **90**（INTID 122），`hdmi_cec` 为 SPI **146**（INTID 178）。`disp_init` 用 `drv_osal_request_irq` 注册 `disp_isr_main`，参数 `0x7a` 与 VDP INTID 122 相符，实际 Linux IRQ 号由 DT 名称查找取得。原厂 TX 有 HDMI 工作线程和状态轮询，DDC 与 PHY 在 DT 中没有独立 IRQ 项。上表映射大小来自具体 `ioremap` 调用。[原厂 DT 摘要](logs/04-factory-dtb-summary.txt)

VDP IRQ 全局寄存器为 `VDP+0x04` 状态、`+0x08` 清除、`+0x0c` mask。原厂 `vdp_disp_{get_int_sta,clear_int_sta,set_int_mask}` 分别读并掩码、写入待清位、读改写开启位。现场状态 `0x7`，mask `0`，原生驱动还需复核 HD 通道各位与前后沿时序。[反汇编和原厂 DT IRQ 属性](logs/247-hdmi-stock-register-disassembly.txt)

GFX 控制和 FDR 以不同的间距寻址。`vdp_gfx_set_layer_enable` / `vdp_gfx_set_upd_mode` 使用 `VDP+0x5000+layer*0x800` 的 bit 31 / bit 29，`vdp_gfx_set_reg_up` 在同一块的 `+0x04` 写入 `1`。FDR 使用 `VDP+0x8a00+layer*0x200`，其 `+0x20` 的 bit 0 为 2D 数据 SMMU bypass，bit 2 为 2D header bypass。现场 GFX0 FDR `+0x20` 为 `1`。这些字段来自原厂函数，Linux 接管实验未改写它们。[现场读数](logs/226-hdmi-uboot-registers.txt) · [GFX 与 FDR 反汇编](logs/247-hdmi-stock-register-disassembly.txt)

### DDC、HPD 与 PHY

| 寄存器 | 位与作用 |
| --- | --- |
| AON `+0x28` | bit 0 HPD，bit 1 PHY RxSense。原厂读值 `0x1`，接采集卡冷启动读值 `0x3`。 |
| AON `+0x40` | `[12:4]` DDC speed count。 |
| AON `+0x54` | bit 0 I²C NACK，bit 1 总线拉低。 |
| AON `+0x5c` | bit 0 SCL 状态、bit 1 SDA 状态、bit 2 SCL OEN、bit 3 SDA OEN。 |
| TX 控制 `+0x38` / `+0x3c` | DDC FIFO 数据出/入，低 8 位。 |
| TX 控制 `+0x40` | `[4:0]` FIFO 计数、`[17:8]` 传输计数。 |
| TX 控制 `+0x44` | `[7:0]` slave address、`[15:8]` offset、`[23:16]` segment。 |
| TX 控制 `+0x48` / `+0x4c` / `+0x50` | 状态、低 4 位命令、清除错误。状态 bit 0 NACK、bit 1 bus-low、bit 2 in-progress、bit 5 FIFO-empty、bit 7 FIFO-full。 |
| TX 控制 `+0x68` / `+0x6c` / `+0x70` | CPU DDC 请求、确认和仲裁状态。 |
| PHY `+0x64` | `[3:0]` channel PD、bit 4 BIST PD、`[8:5]` driver PD、`[10:9]` LDO PD、`[18:11]` termination、bit 19 RxSense PD、bit 20 TXPLL PD。采集卡连通时读 `0x000009f0`。 |

依据为 `hdmi_reg_*ddc*`、`hdmi_reg_pwd_*`、`hdmi_reg_gc_*` 的字段读写，以及 `hal_hdmi_ddc_edid_raw_get` 和 `hal_hdmi_ddc_issue`（`0xc085c29c` / `0xc085b84c`）的 EDID 读取路径。PHY 电源由 `hal_hdmi_phy_power_set`（`0xc086a818`）处理，发射通道使能及端接另由 `hal_hdmi_phy_oe_set`（`0xc086a8d4`）处理。[U-Boot 状态读数](logs/233-hdmi-uboot-tx-timing.txt) · [采集卡接入读数](logs/240-hdmi-uboot-capture-card.txt)

### 时钟、复位与原厂初始化

| 寄存器 | 活动态读数 | 反汇编确认的字段 |
| --- | --- | --- |
| CRG `+0xd8` | `0x400d0019` | VDP/通道时钟。`vdp_open_clk_and_init_crg_config`（`0xc066b780`）置 `0x19`、bit 16、bit 30，在该初始化分支清 bit 6、`[19:18]`、bit 27/28，再在后续步骤清 bit 29。`vdp_disp_select_chan_clk_div` 确认 `[19:18]` 为通道分频字段。活跃态的具体路径另见 `chip_specific_disp_set_chan_clk`。 |
| CRG `+0x34c` | `0x00104000` | VDP 工作时钟控制。原厂初始化清 bit 29，再置 bit 14、20、22，各阶段包含 barrier 和延迟。`chip_specific_disp_set_vdpwork_clk` 对同字执行频率相关选择。 |
| CRG `+0x10c` | `0x00012437` | TX 门控 bit 0 SSC 输入、1 SSC bypass、2 24 MHz、4 OS、5 AS、16 pixel，复位请求 bit 6 总线、7 控制、9 SSC，bit 19 SSC bypass 时钟选择。其余置位的含义仍需确认。 |
| CRG `+0x110` | `0x00000001` | bit 0 PHY TMDS 时钟，bit 4 HDMI PHY reset，bit 5 TMDS reset。 |
| `0xf8000048` | 原厂其它芯片分支映射 | `hdmi_reg_hdmi_*` 在该字操作 bit 19 CEC 门、20 CEC reset、21 OSC 门、22 APB 门、23 总线 reset、24 HDMI reset。`drv_hdmi_prod_io_cfg_set` 另有芯片型号 4 的引脚配置路径，清 `0xf800022c/230/234/238` 的低 4 位。 |

已还原的调用顺序如下。现场采样用于复核活跃态，原厂函数的掩码保留了其它时钟字段和芯片分支。

1. VDP 打开工作域和通道时钟，初始化状态和输出通道，按模式选择 PLL、分频和 HD0 路由。GFX0 设线性显存地址、格式、矩形和 stride，更新后由活动寄存器回读。
2. TX 先映射 CRG、AON、音视频、控制与数据包窗口。`drv_hdmi_prod_crg_init`（`0xc0870128`）准备 I/O，打开门控，脉冲复位，读改写 `0xf8ce4808` 置 bit 18/9，并清 `0xf8ce0850`。U-Boot 同样在 `0x00c80a28`–`0x00c80a48` 出现这两次写入。
3. 原厂 HAL 初始化 DDC、SCDC、PHY 和 TX 视频路径，按模式设置 AVI、TMDS 参数和颜色转换。PHY 先取消 PLL、RxSense、LDO 的掉电，再经 `hal_hdmi_phy_oe_set` 配置端接和输出。复位、PLL 锁定、InfoFrame 和解静音的逐模式时序仍需实测，包括插拔后的重复初始化。

## 3. 原厂 eMMC 显示二进制

在主线 `mmcblk0/ro=1` 下，使用 `O_RDONLY` 从厂商 `blkdevparts` 的边界读取 `vendor` 和 `system` 到构建服务器。`vendor` 起点 `0x8a400000`、长度 400 MiB，SHA256 `726274205f81c1d79ab85da9c7c5f4cf10e8462cbc77b01a5a2a3edf440740b0`。`system` 起点 `0x0d400000`、长度 1200 MiB，SHA256 `06cf51fffea3214b38945c5c322334ccb4456f87b7c381db86442811e0ef1206`。服务器上的副本位于 `/home/hedc/hg680kd-mainline/analysis/hdmi/blocks/`，通过 `debugfs` 只读列目录并提取相关文件。[分区读取串口](logs/244-hdmi-color-bars-and-partition-transfer.txt) · [分区 SHA256 与目录记录](logs/237-hdmi-readonly-partition-analysis.txt)

- `vendor:/lib/modules` 里的八个模块均属于无线、蓝牙或 SDIO 检测。`system` 的常见模块目录为空或不存在。原厂 boot 内核内建 VDP、GFBG 和 HDMI TX，包含上文列出的寄存器访问函数。
- `vendor:/lib/libhi_msp.so` 是 ARM ELF32。反汇编显示 `mpi_hdmi_init` 打开 `/dev/hi_hdmi`，`mpi_hdmi_open/start` 的 `0x2bcb6` / `0x2c02a` 调用 `ioctl@plt`。`mpi_disp_init/open` 同样经 `/dev/hi_disp` 与内核交互。`hidisp` 和 displayclient 走 Android HIDL。
- `system:/system/bin/hdmi_debug.sh` 引用了 `/proc/msp/disp1` 与 `/proc/msp/hdmi0`。此次只离线审阅其内容。原厂用户态二进制的 SHA256、符号和反汇编调用点见记录。[显示二进制审阅](logs/246-hdmi-vendor-display-binaries.txt)

## 4. 主线 DRM 评估

原厂使用海思 TX 寄存器 ABI。它用 32 位位域和分离的 AON、DDC FIFO、视频路径与 PHY 窗口。主线 Synopsys `dw-hdmi` 使用不同的寄存器编号和操作方式，例如 `FC_INVIDCONF=0x1000`、`PHY_STAT0=0x3004`、`I2CM_SLAVE=0x7e00`。原厂在这些功能上直接读写自己的窗口，单独配置地址偏移或 MMIO 宽度无法兼容。适配结论针对软件接口，内部 IP 来源无法从这些寄存器唯一确定。[主线 DW-HDMI 寄存器定义](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/bridge/synopsys/dw-hdmi.h)

原生最小实现可以先保留现有 HDMI link，针对 MV320 实现一个 GEM/DMA 主平面与一个 atomic CRTC，读取活动通道时序，只接管 GFX0 地址、格式、pitch、更新触发和 VDP vblank。首次接管应在完成资源检查之后移除重叠的 simpledrm aperture，保留原固件显存预留。实现前还需确认活动通道的中断状态、清除与更新语义，测试 VDP DMA 地址宽度及 SMMU bypass，再验证 page flip 和原有 Lima 的 PRIME buffer。异型号 Hi3798CV200 已有类似的固件交接式 VDP 实验，可参考其 DRM 拆分与接管顺序，HG680KD 的 GFX/FDR 布局和活动通道应以本机证据为准。[HiMedia Q10 Pro VDP DRM 设计记录](https://github.com/olragon/himedia-q10-pro-linux/blob/main/docs/261011-himedia-vdp-drm-port.md)

独立 modeset 需要单独的 VDP 时序/混合器驱动和海思 TX bridge，后者接入本机 DDC/EDID、HPD、PHY、时钟和复位。在验证端到端 720p/1080p 模式及热插拔之前，本次产物使用固件交接路径。`simpledrm` 的固定输出报告 `connected`，实际 HDMI 链路由采集卡验证。
