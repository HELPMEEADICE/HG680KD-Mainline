# Hi3798MV320 HDMI TX 静态逆向与接管方案

基线为 HG680KD 的 Linux 7.3.6。仓库提交 `f3970e1` 验收 MPV V4L2 Request 和 VDP 零拷贝，`9223587` 记录 USB 全媒体基线。静态取证使用工作区既有日志、原厂 ARM32 内核、kallsyms、DTB 和 Linux 源码。原厂 SDK 构建标识为 `HiHuanglong_V100R009C01SPC023_v2024111912`，通过其板载驱动产物核对实现，未取得同版 SDK 的完整源树或板级原理图。第 6–9 节记录 COM4 TFTP 纯 RAM 探针、SDR 切档与接收端回读。 [USB 全媒体基线](logs/539-usb-7.3.6-full.txt) · [MPV 验收](logs/540-mpv-v4l2-drm-acceptance.txt) · [原厂 SDK 产物来源](VPU-REVERSE.md) · [Git 验收摘录](logs/541-hdmi-ram-acceptance.txt)

**结论。** DDC/EDID 和 HPD 轮询已有足够的寄存器依据，可先在固件链路上接入。第 7–9 节记录了 1080p60 RGB8、2160p30 RGB8 与 2160p60 YCbCr 4:2:0 8 bpc 的原生切档。24p、23.976p 和高于 340 MHz 的 TMDS 链路待实测。HDR10 静态包的具体槽位已定位。端到端 10 bpc PQ 需要验证 DHD0 至 TX 的位深与色彩、sink 能力以及高带宽链路。CEC 有片内控制器、门控和复位，HG680KD 插座侧的电气连通性待验证。海思公开规格支持 HDMI TX 4K60 和 10-bit 解码，可用作芯片能力交叉核对。 [海思 Hi3798MV320 产品规格](https://www.hisilicon.com/cn/products/smart-media/STB/Hi3798MV320)

本文的 **回读** 指既有实机日志，**反汇编** 指可以从原厂二进制复核的指令或数据，**推导** 指按已知字段、协议或时序计算所得，**待验证** 指缺少本板动态证据。原厂内核 SHA-256 为 `a47592ad5a842035f0bd6e41fdce71cb89c2238aff57e2b333d60213af176724`，DTB SHA-256 为 `e289000306072f9fd2e140dc5a837fb646b9c803dae15c2b4d0b99c32cf63517`。复核指令用 A32 小端，VA 减 `0xc0008000` 得平坦映像文件偏移。文中的 `c0…` 和 `c1…` 为原厂虚拟地址，MMIO 用物理地址。 [原厂内核](artifacts/vpu-reverse-factory/hg680kd-stock-kernel.bin) · [kallsyms](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt) · [原厂 DTB](artifacts/vpu-reverse-factory/inputs/factory.dtb) · [反编译 DTS](artifacts/vpu-reverse-factory/out/factory.dts)

## 1. HDMI TX 架构与当前接管边界

数据链是 VDEC DMA-BUF → V0 FDR → VP0 / HD0 mixer（另混入 GP0/G0）→ DHD0 → TX Video → Packet/TMDS → PHY。音频由 AIAO TX2 经片内 I²S 进入 TX Audio，TX 以 TMDS 字符率生成 ACR。HPD、DDC 线状态及 CEC 事件进入 AON，EDID/SCDC 事务由 TX 控制窗口的 DDC master 发起。海思 TX 的 32 位寄存器布局与主线 `dw-hdmi` 的寄存器 ABI 不同，适合实现专用 bridge。 [HDMI 物理窗口及厂商调用](HDMI-REVERSE.md) · [AIAO 路径](AIAO-HDMI-PLAN.md)

### 1.1 硬件资源总表

下表的长度是原厂映射长度或当前 DT 资源长度，IRQ 以原厂 `/soc/virtdev` 的命名与索引为准。SPI 是设备树编号，INTID = SPI + 32，Linux IRQ 由中断域分配。四个列出的 SPI 均为高电平触发。 [原厂 DTS](artifacts/vpu-reverse-factory/out/factory.dts#L1134) · [原厂映射反汇编](logs/247-hdmi-stock-register-disassembly.txt)

| 资源 | 物理地址 / 长度 | 中断 / 关键字段 |
| --- | --- | --- |
| VDP / DHD0 | `0xf8cc0000 / 0x10000`，DHD0 `0xf8ccc200` | SPI 90 / INTID 122。IRQ `+0x04/08/0c`，VT1 bit 0，HD0 underflow bit 3。VT1 阈值 `+0xc228`，消隐状态 `+0xc2a0[1]`。 |
| TX 控制 / DDC master | `0xf8ce0000 / 0x134` | HDMI SPI 88 / INTID 120。`+0x10` 子块复位，`+0x38–0x70` DDC，`+0x104/108` 中断汇总状态与 mask。DDC 以轮询完成。 |
| TX 视频 | `0xf8ce0800 / 0x1ac` | CSC、采样、同步、位深处理及像素/TMDS 时钟检测。 |
| TX 音频 | `0xf8ce1000 / 0x54` | I²S、静音、IEC 60958、ACR。`+0x40/48/4c/50` 为 ACR 控制、N、软件 CTS、硬件 CTS。 |
| TX 数据包与编码 | `0xf8ce1800 / 0x284` | AVI `+0x18`，Audio `+0x3c`，HDR DRM 走 GEN `+0xa8`，GCP `+0x20c`，HDMI 2.0 编码与加扰 `+0x260`。 |
| TX AON | `0xf8ce4000 / 0x64` | HPD / RxSense `+0x28[1:0]`，事件 mask / W1C `+0x30/34`，DDC 线状态 `+0x54/5c`。 |
| TX CEC | `0xf8ce4800 / 0x68` | SPI 147 / INTID 179，收发事件也在 AON `+0x34[13:4]`。原厂常规收发使用轮询回调。 |
| HDMI PHY | `0xf8d30000 / 0xc0` | `+0x10` DPHY reset，`+0x50/54` 时钟图样，`+0x64–0x78` 通道、电源、端接和驱动，`+0x8c–0xbc` TXPLL/SSC。 |
| CRG | `0xf8a22000 / 0x4c4`（原厂 HDMI 映射） | `+0x28/2c` HPLL，`+0xd8/34c` VDP 源、分频和工作时钟，`+0x10c/110` TX/PHY 门控、分频、复位。 |
| HDMI 辅助时钟及 I/O | `0xf8000048 / 0x4`，`0xf8000210 / 0x40` | 辅助字 bit 19 CEC gate、20 CEC reset、21 OSC gate、22 APB gate、23 bus reset、24 HDMI reset。原厂芯片类型 4 执行 Pad mux 初始化，见第 4 节。 |
| AIAO / TX2 | `0xf8cd0000 / 0x10000`，TX2 `0xf8cd2200` | SPI 84 / INTID 116，CRG `+0x118` bit 0 gate、bit 4 reset。 |
| HDCP / HDMI MCU | `0xf8ce2000 / 0xfc`，`0xf8cfc000 / 0x3c0` | 厂商额外窗口。首版使用无内容保护的 HDMI 模式，可按第 4 节另行检查 CEC 待机所有权。 |

CRG `+0x10c` 的 gate 为 bit 0 SSC 输入、1 SSC bypass、2 24 MHz、4 OS、5 AS、16 pixel，复位请求为 bit 6 总线、7 TX 控制、9 SSC，`[13:10]` 是 SSC 分频编码，bit 19 选 SSC bypass clock。`+0x110[0]` 为 PHY TMDS gate，bit 4/5 为 PHY/TMDS reset，`[8:6]` 为 TMDS 分频编码。`drv_hdmi_prod_crg_div_set`（`c086fa70`）对两个表内除数都先减 1 再写字段。历史活动值为 `0x00012437/0x00000001`，两处分频字段解码为 ÷10、÷1，与固件 420 8 bpc 所选表项一致。按字段更新共享 CRG，保留其它位及设备的时钟所有权。 [原厂时钟与复位记录](HDMI-REVERSE.md#时钟复位与原厂初始化) · [kallsyms 中的 CRG accessor](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

### 1.2 固件交接与最小 DRM 接管面

- 当下 `mv320_firmware_mode()` 读取 CRG 和 HD0 的使能、活动尺寸及总计，只匹配两组内建 60 Hz 时序。`mv320_mode_valid()` 只放行固定逻辑 mode，`mode_fixup()` 将 adjusted mode 指向既有扫描时序。单个 HDMI-A connector 提供一个 preferred mode，没有硬件 `detect`、EDID 或 bridge。内核已管 G0/V0、mixer 部分字段、VT1 IRQ 和更新锁存，U-Boot 保有 DHD0 物理时序、HPLL/路由、TX/PHY 和视频包的模式。USB 验收的 1920×1080 KMS 桌面与 3840×2160p60 HD0 同时成立，信号为 VIC 97、YCbCr 4:2:0、8-bit SDR，297 MHz TMDS 字符率。 [VDP 固定模式与 connector](src/hi3798mv320-vdp.c#L761) · [U-Boot 点查](logs/240-hdmi-uboot-capture-card.txt) · [音频链路交接](AIAO-HDMI-PLAN.md)
- **先接管 connector。** 最小窗口为 AON `+0x20/24/28/30/34/40/54/5c` 与 TX 控制 `+0x38–0x50、+0x68–0x70`。给 bridge/connector 提供 HPD detect、DDC adapter/EDID、缓存失效和 hotplug 通知。首个 RAM 验证仍以固件 mode 做硬件 allowlist，物理时序切换进入下一阶段。
- **原生 SDR modeset 的增量。** CRTC 增加 DHD0 基本时序、HDMI 路由、画布/裁剪、VT1 及 HPLL/CRG。bridge 增加 TX 子块 reset，Video 窗口的 mux/同步、CSC、采样和位深，Packet `+0x00/04/18–38/1a4/208/20c/260` 的打包/稳定/AVI/模式/GCP/编码，PHY 的输出隔离、时钟及经验证的频段表。用 `drm_bridge` 的 atomic 时序钩子协调 TX 和 CRTC，把 EDID mode、格式、bpc 和 TMDS 预算收敛为单一原子状态。保留现有 G0/V0 framebuffer、dma-buf 与翻页 ABI。
- **同一 TX 由一个驱动管 MMIO。** 当前 AIAO DTS 已占用 `0xf8ce0010`、`0xf8ce1000`、`0xf8ce1800`，再给 bridge 映射整块会冲突，共享写入也会干扰 ACR 和包。后续将 TX 音频、子复位和包配置收归 TX，由 bridge/HDMI 音频回调协调，AIAO 管 TX2 PCM。DTS 应使用命名资源、CRG syscon 和 VDP 到 TX 的 graph endpoint。热插拔、DDC 和音频回调可能并发，TX 提交需要共享状态锁和有界等待。 [7.3.6 AIAO DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-aiao.dts) · [AIAO 固定链路初始化](src/hi3798mv320-aiao.c#L123)

### 1.3 DDC / EDID 与 HPD

| 窗口 | 最小字段与作用 |
| --- | --- |
| AON `+0x20/24/28` | `+0x20[0]` HPD filter enable、`[1]` override、`[2]` polarity、`[3]` soft value。`+0x24` 两个 16 位阈值，低半字 high threshold，高半字 low threshold。`+0x28[0]` HPD、`[1]` RxSense。 |
| AON `+0x30/34` | 同位序 mask / W1C。原厂 `ctrl_mach_hpd_callback`（`c085cb24`）轮询 bit 0 HPD、bit 1 RxSense，分别 W1C 后读取状态。 |
| AON `+0x40/54/5c` | DDC speed count `[12:4]`。`+0x54[0/1]` 为 NACK/bus-low。`+0x5c[0/1]` SCL/SDA 状态，`[2/3]` SCL/SDA OEN，原厂恢复路径使用。 |
| TX 控制 `+0x38/3c/40/44` | 8 位 FIFO 出/入，`+0x40[4:0]` FIFO 计数、`[17:8]` 传输字节数，`+0x44[7:0]/[15:8]/[23:16]` 为 slave/offset/segment。 |
| TX 控制 `+0x48/4c/50` | 状态 bit 0 NACK、1 bus-low、2 busy、5 FIFO-empty、7 FIFO-full。命令 `+0x4c[3:0]`，`+0x50` 错误清理。 |
| TX 控制 `+0x68/6c/70` | 请求和应答各 bit 0，加仲裁状态。原厂 `hal_hdmi_ddc_issue`（`c085b84c`）等待 ACK，核对 SCL/SDA，命令 `9` 清 FIFO，按事务命令执行，错误用 `0xf` 中止，等 idle 后归还仲裁。 |

原厂以 8 位地址 `0xa0` 表示 EDID（Linux I²C 7 位 `0x50`），`0xa8` 表示 SCDC（`0x54`）。`hal_hdmi_ddc_edid_raw_get`（`c085c29c`）用命令 `2` 读取 128 字节块，先 segment 0 的偏移 `0x00/0x80`，用命令 `4` 读后续 E-DDC segment。按 E-DDC 规则，块 n 的 segment 是 `n >> 1`，offset 是 `(n & 1) * 128`，常规 segment pointer 的 7 位地址为 `0x30`。适合实现能接收组合 I²C 消息的受限 adapter，DDC/SCDC 共用互斥，首版以有限次超时、NACK 退出、完整长度和 checksum 校验为准。原厂 DT 的通用 I²C 在 `0xf8b10/11/12000` 各自独立，DDC 走 HDMI 专用线路。 [原厂 DDC 记录](HDMI-REVERSE.md#ddchpd-与-phy) · [原厂 DTS](artifacts/vpu-reverse-factory/out/factory.dts#L749)

SPI 88 是原厂 DT 给出的 HDMI 资源。已查到的 HPD 路径使用 `hal_hdmi_ctrl_init` 注册并启动状态机及开启 AON bit 0，并未在所查 HDMI 函数中发现直接注册普通 TX IRQ 的调用。先用 AON HPD 状态做 debounced detect/轮询，RxSense 用于诊断端接。独立插拔测试记录 `+0x28` 与 `+0x34` 边沿、SPI 88 计数和 W1C 清除后，再切 IRQ 驱动的 hotplug。HPD 下降清 EDID 缓存、撤销链路和 CEC 物理地址，上升重读能力并重新建立所选模式的 SCDC。 [原厂 DDC/HPD 及板上回读](HDMI-REVERSE.md#ddchpd-与-phy)

## 2. 动态分辨率与帧率切换

### 2.1 DHD0、像素时钟和 PHY

`pf_set_chn_base_timing`（`c05dbe6c`）按逻辑 HD 通道选择 DHD0 寄存器索引 0，先复位/准备通道、写同步和 VT，再调用 `chip_specific_disp_set_chan_clk`（`c0667c38`），最终触发 `vdp_disp_set_reg_up`。逻辑通道值 1 进入 HPLL 编程路径。`vdp_disp_open_intf`（`c05ef2a4`）写 `VDP+0xc200[4]` progressive 和相关时序字。对 progressive 模式，`+0xc208/210` 的低 16 位是 V/H active−1，高 16 位为包含同步脉冲的 V/H 后肩计数−1，`+0xc20c/214` 低 16 位为 V/H 前肩−1，`+0xc220` 低 16 位、`[23:16]` 为 H/V sync 宽−1。`+0xc200[31]` 使能，`[0]` 更新，`+0xc228` 重新布置 VT1。路由取证点有 `+0xc004` mux、`+0xc600[6]` HDMI 模式和 `vdp_disp_set_sync_inv`（`c05ef53c`）。同时按新活动区设置 G0/VP0/mixer，原 G0 1080p 逻辑画布的 2 倍关系只适用于现行 UHD 交接。 [VDP 现有回读和代码注释](src/hi3798mv320-vdp.c#L828) · [原厂 4400×2250 回读](logs/240-hdmi-uboot-capture-card.txt) · [VDP 画布实验](VDP-KMS-PLAN.md)

`get_format_cfg_para`（`c0667228`）从 `g_disp_format_param`（`c1765228`）按 `0x90` 字节取一行。行内 `+0x4c/50` 经 `chip_specific_disp_set_pll`（`c06679e0`）写入 `CRG+0x28[30:0]/+0x2c[27:0]`，`+0x54` 为格式编号。按同一 CRG 上已解出的 APLL 位序对表解码，HPLL 名义频率可用 `Fhpll = 24 MHz × (fbdiv + frac / 2^24) / (refdiv × post1 × post2)`。候选字段为 `+0x28[23:0]` frac、`[26:24]/[30:28]` post1/post2，以及 `+0x2c[11:0]/[17:12]` fbdiv/refdiv。所有下表整数档得 1188 MHz，小数档得约 1186.812 MHz。`select_channel_pll_source`（`c0667908`）按 AON HPD 和模式选择 `CRG+0xd8[16]`，`chip_specific_disp_set_chan_clk` 另写 `+0x34c[28:24]` 和 `+0xd8[19:18]`。 [同 SoC CRG 位序旁证](DVFS-THERMAL-REVERSE.md#cpu-时钟和原厂-opp) · [原厂符号表](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

| CTA 目标 | Htotal × Vtotal | 像素时钟 MHz | 原厂时序行 VA / 格式编号 | HPLL 原始两字 | `34c[28:24] / d8[19:18]` | 推导总分频 |
| --- | --- | --- | --- | --- | --- | --- |
| 1080p60 / VIC 16 | 2200×1125 | 148.500 | `c17652b8 / 0x00` | `11800000 / 00001031` | `1 / 0` | 8 |
| 2160p60 / VIC 97 | 4400×2250 | 594.000 | `c1766b78 / 0x41` | `12000000 / 00001063` | `0 / 3` | 2 |
| 2160p30 / VIC 95 | 4400×2250 | 297.000 | `c1766a58 / 0x3f` | `12000000 / 00001063` | `1 / 3` | 4 |
| 2160p24 / VIC 93 | 5500×2250 | 297.000 | `c1766938 / 0x3d` | `12000000 / 00001063` | `1 / 3` | 4 |
| 2160p23.976 / VIC 93 | 5500×2250 | 296.703297 | `c1766ed8 / 0x47` | `12e6a7f0 / 00001062` | `1 / 3` | 4 |
| 1080p24 / VIC 32 | 2750×1125 | 74.250 | `c17654f8 / 0x04` | `12000000 / 00001063` | `3 / 0` | 16 |
| 1080p23.976 / VIC 32 | 2750×1125 | 74.175824 | `c17671a8 / 0x4c` | `12e6a7f0 / 00001062` | `3 / 0` | 16 |

表中总分频是 `Fhpll / (Htotal × Vtotal × Hz)` 的推导。各行吻合 `D_eff = 2 × (1 + CRG34c_select)`（UHD 路由）或 `4 × (1 + CRG34c_select)`（FHD 路由）。固定因子的具体位置与 `+0xd8` 编码语义仍需量测，按原厂模式表落地比暴露任意 PLL 设频更可控。帧率以 `24000/1001` 表示，保持原 24p totals，原厂 frac 字反算分别是 296.703000/74.175750 MHz，约低 1 ppm。历史活动 CRG `0x400d0019/0x00104000` 与 UHD 的编码一致。HPLL 的锁定位已由第 8 节定位，`sync_is_hpll_tune_freq_busy` 是常量返回，不能用于锁定判断。 [Linux 7.3.6 CTA 模式表及分数频率处理](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/drm_edid.c) · [既有 CRG 回读](logs/233-hdmi-uboot-tx-timing.txt)

PHY 的频率上限按 TMDS 字符率判断。对 RGB/YCbCr 4:4:4，`Fchar = Fpixel × bpc / 8`，对 4:2:0 再除以 2，4:2:2 使用自身打包，`Fchar = Fpixel`。三条数据 lane 各以 `10 × Fchar` 串行发送，时钟 lane 的频率为 `Fchar`（1/10）或 `Fchar / 4`（1/40）。这是输出目标公式。原厂 PHY 带输入、环路、后级以及 CRG 分频，常规 PLL 关系可写作 `Fvco = Fref_PHY × N_eff / R_eff`，并经有效 post divider 和串行器得到 lane 速率。已定位字段及表驱动配法，`N_eff/R_eff` 的编码、参考源频率、SSC 与后级的组合公式仍未由反汇编闭合。 [Linux 7.3.6 HDMI 时钟 helper](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_hdmi_helper.c)

| PHY 字段 | 静态依据 |
| --- | --- |
| `+0x10[0]`，`+0x64` | DPHY reset。`+0x64[3:0]` channel PD、`[4]` BIST PD、`[8:5]` driver PD、`[10:9]` LDO PD、`[18:11]` 端接、`[19]` RxSense PD、`[20]` TXPLL PD、`[22:21]` divsel。原厂 `hal_hdmi_phy_power_set/oe_set` 分开处理供电和驱动。 |
| `+0x68–0x78` | CLK/D0/D1/D2 各 lane 主驱动、预加重、端接。`hal_hdmi_phy_spec_set`（`c086addc`）查驱动参数并调用各 accessor。模拟实测、温度边界和 trim 耦合待确认。 |
| `+0x8c / 90` | `+0x8c[1:0]/[4:2]/[11:5]/[13:12]/[17:14]` 为 clkin、输入、环路、后级分频和电荷泵选择，bit 18/19 为 ref/fb ÷2 和 SSC divider 使能。`+0x90[19:0]` 对应 TXPLL test 字。 |
| `+0xb0/b4/b8/bc` | SSC/fb。`+0xb0[23:0]` 是 `set_fb`，bit 24 load 脉冲，bit 25–27 tune/clear/mode，后续字用于步长、阈值、跨度及间隔。 |
| `+0x50[18:9] / +0x54[29:0]` | 模拟时钟的高 10 / 低 30 位。`hal_hdmi_phy_tmds_set`（`c086b4f0`）用表内 `enClkRatio` 选择 40 位图样。`0x1f / 0x01f07c1f` 是四组连续 5 个 0 和 5 个 1，`0 / 0x000fffff` 是连续 20 个 0 和 20 个 1，据此分别解为 1/10、1/40。 |

`phy_div_cfg_get`（`c086a368`）从 `g_phy_div_cfg_tab`（`c177e87c`）按时钟范围（kHz）和位深枚举查找 17 行、每行 `0x24` 字节。`hdmi_attr_to_video_cfg`（`c0878508`）对 420 先把像素时钟除 2，`hal_hdmi_videopath_set`（`c0856f3c`）将这一路时钟与输出位深枚举传给 `hal_hdmi_phy_tmds_set`，因此 420 查表使用折半后的 8 bpc 基准，另外用 bpc 区分档位。枚举 0 的 100001–200000、200001–340000、340001–600000 kHz 三行分别给出 `(SSC 除数, TMDS 除数, set_fb, enClkRatio, divsel)` = `(10,2,0x640000,0,1)`、`(10,1,0x640000,0,0)`、`(5,1,0x640000,1,0)`。10 bpc 的枚举 1 在 272000 kHz 处分档，符合 `272 MHz × 1.25 = 340 MHz`。2160p60 420 10 bpc 对应 297000 kHz 档的 `(8,1,0x640000,1,0)`，仍需板上复核。原厂 TX 的 `Packet+0x04[0]` 是 `pclk2tclk_stable`，另有 `Video+0xb8[8]` 启动检测和 `+0xc8/cc` 像素/TMDS 计数，用于 RAM 核验。该 stable 位仅证明 TX 内部状态，验收还须匹配实际刷新、sink 锁定和线端时钟。 [PHY 板上回读](logs/240-hdmi-uboot-capture-card.txt) · [原厂 PLL、PHY 与测频函数](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

### 2.2 SCDC、加扰与模式切换顺序

2160p60 的 8-bit YCbCr420 为 297 MHz，可先复现固件的低于 340 MHz 链路。2160p60 10-bit 420 为 371.250 MHz，2160p60 RGB/444 8-bit 或 422 为 594 MHz，均需高带宽链路。2160p60 RGB/444 10-bit 需要 742.500 MHz，超出 600 MHz TMDS 范围。能力筛选包括 sink 的 HDMI Forum VSDB、max TMDS、SCDC、逐 VIC 的 Y420 能力和 deep-color 标志。 [Linux 7.3.6 HDMI 模式与格式检查](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_hdmi_state_helper.c)

1. 原厂 `hal_hdmi_ctrl_tmds_mode_set`（`c085dd70`）设置 `Packet+0x208[0]` HDMI 模式，高速分支设置 `+0x260[0]` HDMI2 encoder 并清 `[5]` bypass。`scdc_scramble_set`（`c08607e0`）写 `+0x260[1]` source scrambler，同步通过 DDC 写 sink 的 `0x54:0x20[0]` scrambling 和 `[1]` 1/40，目标通常为 `0x03`。本地 clock ratio 由 PHY 图样及对应 CRG/PHY 档协调。`+0x260[2]` 有 `enc_scr_md` accessor，已追到的加扰路径只写 bit 1。
2. 在 HPD 有效时读 sink `0x01` 版本，核对能力后按版本协议设置 `0x02` Source Version。修改 `0x20` 时保留其它位。7.3.6 helper 在写 ratio 后等待 1–2 ms，并注明协议等待窗口为 1–100 ms，故 PHY 与时序准备应放在该写入前。高速启用时验证 `0x20` 配置、`0x21[0]` scrambling status 和 `0x40[3:0]` clock/channel lock。原厂 `hal_hdmi_scdc_attr_set`（`c0860b78`）至少包含关 OE → 写两端 → 等待 → 开 OE → 状态回读和有界重试。切回低速时同步拟定清 `0x20[1:0]`、关 source scrambling、恢复 1/10，处理掉线或 sink 掉电后 SCDC 状态丢失。TX 控制 `+0x14` 还有 auto poll/reply/access 位，首版走软件 DDC，读请求、硬件自动应答及异常时序待核定。 [Linux 7.3.6 SCDC 定义](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/include/drm/display/drm_scdc.h) · [Linux 7.3.6 SCDC 时序与工具](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_scdc_helper.c)
3. 拟定 atomic 顺序为能力和带宽检查 → 音频先静音、发 AVMUTE、停包并关 PHY OE → 停/屏蔽 DHD0 时序 IRQ 与待完成翻页 → 在停输出时准备 DHD0/路由/时钟，按原厂次序写 HPLL、分频后触发更新 → 配 TX Video/Packet、PHY 供电、分频、图样和必要的复位 → 恢复 DHD0/VT1、验证时钟，按所需 SCDC 窗口开 OE 和校验 sink → 送正确 AVI/GCP，重配 ACR 并在稳定后解静音。失败路径保持静音，回退上个已知可用的模式/两端 SCDC 或保持输出关闭。HPD 变更应使同一轮提交失效，避免对新插的 sink 沿用旧能力。

### 2.3 AIAO 与 N / CTS

48 kHz、双声道 S16LE 的 TX2 仍用 12.288 MHz MCLK 和 3.072 MHz BCLK。像素模式变化时可保留这套 I²S 速率、PCM 采样率及 DMA 格式，TX 的 ACR 必须以新 TMDS 字符率重新选择和核验 N/CTS。`Fs = Fchar × N / (128 × CTS)`，1/40 时钟 lane 的线频不可代入。`hal_hdmi_n_value_get/hal_hdmi_cts_value_get`（`c085c854/c085c8e8`）按采样率和字符率 kHz 查 `g_audio_cts_n`（`c177707c`）。原厂 48 kHz 条目包括下列值。 [原厂 AIAO 与 ACR 记录](AIAO-HDMI-PLAN.md) · [原厂符号表](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

| 字符率 MHz | 原厂 N / CTS | 对应例子 |
| --- | --- | --- |
| 74.250 / 74.1758 | 6144 / 74250，11648 / 140625 | 1080p24 / 23.976 RGB8 |
| 148.500 | 6144 / 148500 | 1080p60 RGB8 |
| 297.000 / 296.703 | 6144 / 297000，5824 / 281250 | 2160p30 或 24 RGB8，2160p23.976 RGB8 |
| 594.000 | 6144 / 594000 | 2160p60 RGB8 高速档 |
| 371.250 | 6144 / 371250（按公式推导） | 2160p60 420 10 bpc，需测 ACR 和接收端音频 |

现驱动 probe 绑定 AVI VIC 97/420 和固定 CTS，必须转为 TX 依据当前 atomic link state 提供 ACR。`TX Audio+0x40[0/1/2/4]` 分别有 ACR 请求、HW/SW 选择、CTS 生成选择、使用 SW CTS 位，固件工作组合的低 5 位为 `7`。留存新模式的 `+0x48/4c/50` 回读并复核选择语义。原厂写 N/CTS 后脉冲 `0xf8ce0010[8:6]` 复位 Audio FIFO/ACR/Audio。切换时先静音 TX 音频及禁止旧状态继续发包，必要时由 ALSA 同步暂停 TX2 或以静音样本续送，再写 N/CTS、恢复 Audio InfoFrame 和样本包，待 FIFO/ACR 稳定才解静音。首轮用连续 48 kHz PCM 在模式往返和拔插时检查音频包、采样率、XRUN、爆音及恢复延迟。 [现有 48 kHz 配置](src/hi3798mv320-aiao.c#L123) · [USB 音视频回归](logs/539-usb-7.3.6-full.txt)

## 3. 10-bit 输出与 HDR10 InfoFrame

### 3.1 从 V0 到 HDMI 线端

- **VDP 输入有 10-bit 证据。** V0 的 `V0_FDR_WIDTH_8_PLUS_2` 启用分离的 8/2 位 tiled Y/UV 四面，`DRM_FORMAT_HI3798MV320_TILED10` 与现有 modifier 读 DMA-BUF。V0 CSC 使用 10-bit 偏移与 `0…1023` 裁剪。现有 `COLOR_ENCODING/COLOR_RANGE` 是 V0 输入属性，BT.2020 输入使用向当前 BT.709 limited 输出目标转换的矩阵。这条路径已在 Main10 60.000 fps 实测。 [VDP 的 CSC 与四面配置](src/hi3798mv320-vdp.c#L413) · [MPV Main10 验收](MPV-V4L2-DRM-PLAN.md)
- **原厂路径有高位深处理和 420 双形态。** `disp2_hdmi_vo_attr`（`c0883a24`）把 VDP 输出属性传向 HDMI，`hdmi_attr_to_video_cfg` 区分输入格式/位深及 TX 目标。`disp_set_hdmi420_typeone/typetwo` 有 VDP 侧的 420 配置，`hal_hdmi_ctrl_videopath_set` 另有 TX 采样与 demux 路径。`ctrl_video_dither_set` 分别读入、出位深字段，选择 dither、TMDS packing 和 deep-color packet。原厂 `check_deep_color_10bit` 对 RGB、444、420 查不同能力字段，对 420 路径把折半的基准字符率乘 5/4。以上支持有条件实现 10 bpc 输出。DHD0 物理接口每拍 lane 数、跨 mixer/CSC 的有效位深和各格式的准确位布局仍需实机验证，验收用低两位可区分的渐变/阶梯并读 sink 输入格式与 bpc。 [原厂接口、格式与位宽函数](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

| 控制点 | 寄存器及用途 |
| --- | --- |
| TX 样本排列、同步 | Video `+0x1a8[8:0]` Y/Cb/Cr 选择，`[28:25]` sync invert，`[24:21]` sync mask。RGB、YCbCr444 由 mux 和 CSC 配合决定。 |
| TX 颜色转换 | Video `+0x134[11:4]` CSC mode、`[3:0]` saturate/dither/系数来源/enable，`+0x15c–0x170` 输入/输出 offset。BT.2020 信号需有匹配的矩阵、范围、裁剪和采样位置。 |
| TX 422 / 420 采样 | Video `+0x180[0/2/3]` 横向 downsample、纵向使能/旁路，`+0x18c[0/10]` 420 demux/像素分频，另有 `+0x180[1]` 横向滤波。区分 DHD0 已复用的 420 输入和 TX 自行降采样，按原厂对应分支配置。 |
| TX 位宽和抖动 | Video `+0xec[2:0]` dither mode/bypass、`+0xd4[0]` 2p1 dither enable。Packet `+0x00[1:0]` TMDS pack mode，`+0x208[1]` deep-color packet enable。以 `ctrl_video_dither_set`（`c085cd40`）为配置入口证据。422 用 HDMI 特有的打包，线端总字符率维持 1× 像素率，实际源精度仍要查验。 |

首次确认 HDR 包与真实 10 bpc 可选支持 HDR 的 1080p24/60 RGB10，或 sink 明确公布对应 VIC 及 420 deep color 的 2160p24/30 420 10 bpc（185.625 MHz）。进入高速验证后，2160p24/30 RGB 或 444 10 bpc 与 2160p60 420 10 bpc 都是 371.250 MHz，可用同一字符率比对不同视频路径。支持 422 的 sink 可作为 2160p60 的另一组验证，涉及 594 MHz PHY 档和 422 打包。4K60 RGB/444 10 bpc 在 600 MHz 限制下应拒绝。 [Linux 7.3.6 TMDS 公式](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_hdmi_helper.c) · [Linux 7.3.6 sink 能力检查](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_hdmi_state_helper.c)

### 3.2 AVI 与 HDR 静态包

包窗口基址为 `0xf8ce1800`。原厂写法统一为 header 的 HB0/1/2 在同一 32 位字的低 24 位。接下来的 PB0 是 checksum，每组 7 个 PB 写两字，低字 4 字节，高字低 24 位 3 字节。`drm_hdmi_infoframe_set_hdr_metadata()` 与 `hdmi_drm_infoframe_pack()` 可生成 30 字节的标准 header、checksum 和 26 字节 payload，尾部未使用的 PB 置零。 [Linux 7.3.6 DRM metadata helper](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_hdmi_helper.c) · [Linux 7.3.6 包编码](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/video/hdmi.c)

| 槽位 | 头 / 负载（偏移） | 使能 / 重发 |
| --- | --- | --- |
| AVI `0x82` | `+0x18 / +0x1c–38` | `+0x1a4[0/1]` |
| Audio `0x84` | `+0x3c / +0x40–5c` | `+0x1ac[0/1]` |
| SPD `0x83` | `+0x60 / +0x64–80` | `+0x1a8[0/1]` |
| **GEN，HDR DRM `0x87`** | **`+0xa8 / +0xac–c8`** | **`+0x1b4[0/1]`**，活动状态 `+0x1e0[3]` |
| GEN3，原厂 `0x81` 分支 | `+0xf0 / +0xf4–110` | `+0x1c0[0/1]` |
| Gamut / 另一路 VSIF | `+0x15c / +0x160–17c`，`+0x180 / +0x184–1a0` | `+0x1cc[0/1]`，`+0x1d0[0/1]` |
| GCP | `+0x20c[0]` Set AVMUTE、`[1]` Clear AVMUTE | 深色包配置见 `+0x208[1]`，AVMUTE 用原厂的 `hal_hdmi_ctrl_avmute_set` 路径复核。 |

- **AVI。** `+0x1c[15:13]` 为 DB1 的 Y，RGB/422/444/420 分别为 0/1/2/3。BT.2020 非恒定亮度 YCC 或 RGB 使用 DB2 C=3（`+0x1c[23:22]`）、DB3 EC=6（`+0x1c[30:28]`），恒定亮度 YCC 的 EC=5。`+0x20[7:0]` 是 DB4 VIC，quantization/range 随实际像素路径。包内字段变化应重算 checksum。 [Linux 7.3.6 AVI 打包与 colorimetry](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/video/hdmi.c) · [现有 AVI 回读和校验](HDMI-REVERSE.md#1-logo-显存与-hdmi-时序)
- **HDR DRM。** `drv_hdmi_drm_infoframe_send`（`c0871758`）提交 type `0x87`。`hal_hdmi_ctrl_infoframe_data_set`（`c085e11c`）的 `0x87` 跳表项指向 `c085e254` 的 GEN 路径，`hal_hdmi_ctrl_infoframe_en_set` 在 `c085e064` 开 GEN 和 repeat。头 `+0xa8=0x001a0187`，`+0xac[7:0]` checksum、`[15:8]` EOTF=2（PQ）、`[23:16]` Static Metadata Descriptor ID=0。DB3–22 装 mastering primaries、白点、最大/最小亮度。DB23–24 MaxCLL 写 `+0xc4[31:16]`，DB25–26 MaxFALL 写 `+0xc8[15:0]`，16 位值按低字节在前，亮度单位按 UAPI/CTA。软件在同一 atomic 更新中暂停槽位、写头/负载及 checksum 后恢复 repeat。SDR 回切清 HDR blob、关闭 GEN 并恢复对应 AVI 和像素处理。需在不同电视确认撤包后退出 PQ，原厂另有 zero DRM packet timer 可作为兼容性线索。 [原厂 HDR 符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt) · [Linux 7.3.6 DRM 包编码](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/video/hdmi.c)

### 3.3 Linux DRM 与 MPV 接入

1. **包编程改动较小。** 用 `DRM_BRIDGE_ATTACH_NO_CONNECTOR` 配合 `drm_bridge_connector_init()`，声明 `DRM_BRIDGE_OP_HDMI` 和 `DRM_BRIDGE_OP_HDMI_HDR_DRM_INFOFRAME`。按 7.3.6 要求提供 AVI、HDMI Vendor、HDR DRM 的 write/clear，接管音频时加 Audio 槽回调。开放标准 `HDR_OUTPUT_METADATA` blob、`Colorspace`、`max bpc`，公布经过验证的 output format。atomic check 依据 EDID 的 PQ EOTF、Static Metadata Type 1、BT.2020、对应颜色格式/bpc 和 TMDS 带宽确定可用组合。仅元数据更改时可在同一链路更新包，输出格式、bpc 或字符率改变时重走 modeset。 [Linux 7.3.6 bridge connector](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_bridge_connector.c) · [Linux 7.3.6 HDMI connector state](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_hdmi_state_helper.c)
2. **输出色彩改动中等。** 把 connector 输出的目标色彩、范围、EOTF 和 bpc 传到 VDP 色彩策略，以输出目标选择 V0 CSC、mixer 和 TX CSC/dither。对 BT.2020 PQ 直通应保留 PQ 样本及低两位。含可见 G0 的 HDR 桌面需要处理 SDR 图层到 PQ 的映射，原厂有 VDP HDR/WCG 块，首版用已验收的静态透明 G0 隔离该问题。
3. **MPV 保留现有零拷贝。** V4L2 Request、4-plane tiled DMA-BUF、H0TV format/modifier、PRIME import 和 V0 page flip 可沿用。将 MPV/FFmpeg 的 primaries、matrix、range、transfer、mastering display 及 content-light 元数据绑定到同一 atomic commit 的 V0 输入属性和 connector blob，复查 MPV DRM 现有 `vo_drm_set_hdr_metadata()` 调用在直扫路径上拿到原片元数据并同时提交 `Colorspace`。只在色彩、格式或 metadata 切换时更新相应状态。退出 HDR 时还原用户态 KMS 状态和 fbcon。验收同时检查 `detile=0`、低两位可区分的 10-bit 图样、线端 InfoFrame、实际 bpc 与色度、掉帧和 PCM。 [现有 MPV DRM 补丁](scripts/mpv-v4l2request-drmprime-overlay.patch) · [MPV 与 V0 验收](MPV-V4L2-DRM-PLAN.md)

## 4. HDMI-CEC

### 4.1 片内资源、原厂 Pinmux 和板级证据

CEC 是集成在 HDMI TX 内、有独立窗口和 IRQ 资源的控制器。`hdmi_tx_cec_regs_init`（`c0855020`）映射 `0xf8ce4800 / 0x68`。直接解析原厂 DTB 两个属性，`hdmi_cec` 的零基索引 32 配对 `<0 0x93 4>`，即 **SPI 147 / INTID 179**。这纠正了 `HDMI-REVERSE.md` 的 CEC 编号，索引 31 的 `<0 0x92 4>` 对应 `SecInvokeirq`。`hdmi_crg_regs_init`（`c086de54`）把 `0xf8000048` 映射到 CRG 指针结构的第二项。CEC gate/reset accessor（`c086ea70/c086eb14`）分别改 bit 19/20，共用 OSC/APB 等门控见第 1 节。`drv_hdmi_prod_crg_gate_set/all_reset_set` 对芯片类型 1–4 访问该字。原厂 `hal_hdmi_cec_init`（`c0865030`）注册参数为 10 的状态机回调，常规 TX/RX 路径由 `cec_mach_callback`（`c08663bc`）读取 AON 状态。SPI 147 的实机递送见第 6.3 节。 [原厂 DTB](artifacts/vpu-reverse-factory/inputs/factory.dtb) · [原厂 DTS](artifacts/vpu-reverse-factory/out/factory.dts#L1134) · [原厂 CEC 及 CRG 符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

HG680KD 的既有回读 `0x37980320` 和 feature `0xc0100000` 匹配 `drv_sys_get_chip_version`（`c055a148`）的 `0x10/0x320` 分支。`drv_hdmi_prod_chip_get`（`c086fae4`）对此返回类型 4，`drv_hdmi_prod_io_cfg_set`（`c086fe18`）明确在类型 4 清 `0xf800022c/230/234/238[3:0]`。因此这组 Pad mux 和辅助 CEC 时钟/复位适用于本机芯片分支。既有 U-Boot 读取 `0xf8ce4800=0xa08014cc`、`0xf8ce4808=0x10020200`，可确认 CEC MMIO 可访问。厂商内核和 U-Boot 的 HDMI 初始化均有对 `0xf8ce4808` 的写入。 [芯片标识回读](logs/406-vpu-uboot-registers.txt) · [CEC MMIO 回读](logs/233-hdmi-uboot-tx-timing.txt) · [HDMI 初始化反汇编](logs/247-hdmi-stock-register-disassembly.txt)

厂商 `/soc/edid` 只给出 `hi3798mv300e.hdmitx` 与 EDID 策略属性，CEC IRQ 也在通用 `virtdev` 下。DTB 中没有 HDMI CEC 的 `pinctrl-0`、GPIO/pin 描述或 Pad 到插座的网络；通用 `hivendor_iocfg` 指向 `0xf8a21000`。静态证据定位了 HDMI Pad 选择路径，四字各自对应 HPD/DDC/CEC 的次序和板上 Pad 实际状态待查。**目前无法确认 HG680KD 的 HDMI Type-A pin 13 已接通、未被复用或未因板修订省件。** 在进入驱动实现前，对实际板号核对原理图或断电查 pin 13 到对应串阻/保护器件/SoC Pad 的连通，再用已知支持 CEC 的 TV 或 CEC 分析仪观察待机上拉、收发与应答。板级缺线会使软件路径失去价值。 [板载 DTS 中的 iocfg 与 edid 节点](artifacts/vpu-reverse-factory/out/factory.dts#L954) · [板载 DTS 的 HDMI 策略](artifacts/vpu-reverse-factory/out/factory.dts#L1032)

### 4.2 最小收发与 Linux CEC 框架

| 窗口 | 字段及原厂依据 |
| --- | --- |
| CEC `+0x08` | bit 30/29 清全部/当前 RX，`[28:26]` retry limit、`[25:22]` operand count，bit 19 manual send、18 pass-through、16/15 校准使能/触发、14 TX FIFO flush、9 NACK header 控制。 |
| CEC `+0x0c` | `[15:12]/[11:8]` RX initiator/destination，bit 7 RX error，`[5:4]` FIFO 状态，`[3:0]` RX count。 |
| CEC `+0x1c/20/24` | `+0x1c[3:0]` initiator LA，`+0x20[3:0]` destination、`[4]` header-only poll，`+0x24[15:0]` capture mask。 |
| CEC `+0x28–34 / +0x38–44` | TX/RX 的 opcode 位于起始字低字节，后续为按字节排列的 operand。Linux 普通 CEC 帧最多 16 字节，含 header 与 opcode 时最多 14 个 operand。 |
| AON `+0x30/34[13:4]` | 事件使能与 W1C。原厂回调使用 bit 11 作 TX 完成，bit 6 作失败路径（poll 路径记录 NAK），bit 12 标记 RX 队列事件，bit 5/7/8/9 等用于 RX 错误。仲裁丢失、重试耗尽及超时的完整映射需注入验证。 |

1. **启用。** 持有 CEC gate 与依赖的 AON/APB 电源域，解除复位，按动作位语义清 RX/FIFO 和 AON W1C。原厂 `cec_hw_enable.part.1`（`c0864df8`）清 RX、清 manual 位并处理 AON bits 4–13。`hal_hdmi_cec_enable` 设置 `initiator` 和 capture mask。只启用已验证的事件源，默认从单一逻辑地址起步。
2. **发送。** 设置 initiator、destination、对应 count、opcode 和 operand，清 TX 旧完成位，按 `cec_manual_cmd_set`（`c0864d40`）对 `+0x08[19]` 触发。无 opcode 的 poll 使用 `+0x20[4]` 的单次路径。由 IRQ 或有界轮询等待结果，按已验证状态区分 ACK、NACK、仲裁失败、错误/超时，失败时复位对应 FIFO 并释放本次事务。依据为 `cec_msg_tx`（`c0864edc`）和 `cec_mach_callback`。
3. **接收。** 读取 AON RX 事件及 FIFO 状态，读取 `+0x0c` 的头和长度及 `+0x38–44` 数据字，校验长度及错误位后清当前条目并按需循环收空，W1C 仅写已消费的事件位。保护共享 AON mask 的 RMW，使 HPD 与 CEC 能并行。RX count 的边界、无 opcode 消息及 capture mask 的广播行为均加入回归。
4. **接框架。** 用 `cec_allocate_adapter()` / `cec_register_adapter()`，实现 `adap_enable`、`adap_log_addr`、`adap_transmit`，以 `cec_transmit_attempt_done()` 或 `cec_transmit_done()` 上报结果，用 `cec_received_msg()` 上报 RX。通过 HDMI CEC helper 或 notifier 在 EDID 更新时设置物理地址，拔插时清失效地址并遵循实测的无 HPD 行为。板端连通通过后，基本收发的改动量为中等，重点在时钟生命周期、IRQ 递送、重试和 signal-free time，待机唤醒另需核对 MCU 与 CPU 的所有权。 [Linux CEC 驱动 API](https://docs.kernel.org/driver-api/media/cec-core.html) · [Linux 7.3.6 bridge CEC 接口](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/gpu/drm/display/drm_bridge_connector.c)

## 5. 实施优先级、风险与最小 RAM 验证

| 优先级 | 范围 | 投入、收益和完成条件 |
| --- | --- | --- |
| **1** | **HPD / DDC / EDID → 原生 SDR modeset → 高速 SCDC** | 先在固件时序上验证 HPD 和 EDID，投入小且可直接复用到 DRM、SCDC、HDR、CEC。接着实现 1080p60 ↔ 2160p30，扩到 2160p60 420 8 bpc，再验 24/23.976。最后独立验证 >340 MHz。主要投入在时钟、PHY、事务回退和音频联动。 |
| **2** | **真实 10 bpc + HDR10** | GEN 包位置已确认，优先用低带宽模式分开验证 10 bpc 与 PQ，再升到 2160p60 420 10 bpc。验收应包含无 CPU detile、有效低两位、AVI BT.2020、DRM PQ/Type 1、MaxCLL/MaxFALL、SDR 回切和持续 PCM。 |
| **3** | **CEC** | 板级连通可提早做快速确认。排期仍放在显示与 HDR 之后，因收益面较窄且这块板的连接未知。通过 pin 13 和实际 ACK 验证后实现标准 adapter，最小验收为对 TV 的 poll/应答、RX 对照及异常恢复。 |

| 风险 | 当前缺口 | 验证依据 |
| --- | --- | --- |
| HPLL / PHY 校准 | HPLL/PHY 真正的锁定位、固定分频的时钟树位置、各 lane 振幅/预加重/端接、电荷泵、trim 与温度条件尚未闭环。原厂表提供起点。 | 每个认可模式保存 CRG、DHD0、PHY 与 TX counter，核对线端时钟、VT1 实测周期和 sink 锁定。分档分步验证，丢锁或 underflow 停止。 |
| SCDC 与 DDC 竞争 | AON、硬件自动轮询、Read Request、NACK/总线拉低恢复以及断连重置需测。历史 4K60 420 8 bpc 路径的字符率为 297 MHz。 | 用具备 HDMI Forum VSDB 的已知 sink，读两端配置、状态与错误计数，检查降速/重插。DDC 限时和串行化，热拔插立即中断事务。 |
| 深色、HDR 和 G0 | DHD0 内部有效 10 位、420/422 采样位置、G0 SDR 合成、GEN 停发行为均需取证。 | 使用可报告 HDMI 输入 bpc/format 的设备或 HDMI 分析仪，比较精确的低位渐变与线端包，现有 1080p30 USB 采集结果只支撑可见画面与 PCM。 |
| AIAO / 并发 | 目前固定 VIC 97、CTS 与独占资源冲突，切换时可能出错或出现旧包。 | 模式切换、连续 PCM、PCM 启停、HPD 同时进行，检查 N/CTS、静音边界、XRUN、帧和资源恢复。 |
| CEC 板级与 IRQ | 第 6 节已确认 SPI 147 递送。Pad 对照、插座连通和完整错误映射仍待验证。 | 断电连通、被动观测、ACK/NACK 双向对照及 RX 溢出。 |

第 6 节覆盖现有直连时序上的 HPD 稳态、完整 EDID、CEC 发帧和中断。后续对支持多扩展块的 sink 验证跨 segment，对宣告 SCDC 的 sink 读取版本和链路状态，并单独验证 HPD 边沿与视频恢复。 [既有 RAM 启动约定](UBOOT-USB-BOOT.md) · [7.3.6 RAM 验收](logs/491-vdp-7.3.6-phase3-acceptance.txt)

EDID 验收后再扩大到 RAM 中受限的 1080p60 ↔ 2160p30 RGB8 SDR 测试，首轮用固定图样和 48 kHz LPCM。每次保存已知可用状态，提交设超时，失败静音并回退或由串口重启回原厂交接。

## 6. 2026-11-28 · Linux 7.3.6 纯 RAM 实机探针

HG680KD 保持 USB2 Video 采集卡直连。软件已验证 HPD/RxSense 稳态、DDC 仲裁和完整 EDID，以及 CEC 发送完成路径的 SPI 147 递送。采集端对 CEC Polling 返回 NACK，板端 pin 13 连通性仍未确认。 [COM4 全程日志](artifacts/arm64-7.3.6-hdmi-probe/board-com4.txt)

### 6.1 镜像与操作边界

在 `hedc@ArchLinux-E5-Server:/home/hedc/hg680kd-mainline` 复制已验收的全媒体 Linux 7.3.6 源码与配置，从空输出目录用 GCC 14.2.0 和 `CONFIG_WERROR=y` 构建，日志零警告。构建脚本复用已验收的 AIAO initramfs，禁用 Linux MMC 和 USB 存储，探针仅编入这一镜像及其 DTS。驱动绑定时映射资源、解析 IRQ 并创建 debugfs，探针的寄存器写入和 IRQ 申请由用户写入 `hi3798_hdmi_probe/run` 触发。命令为 `hpd`（1 秒稳态采样）、`watch`（12 秒边沿采样）、`edid` 和 `cec`。结果经 `report` 读取，`edid` 文件提供校验通过的二进制块。 [构建脚本](scripts/hdmi-probe/build.sh) · [探针](scripts/hdmi-probe/probe.c) · [仅测试镜像使用的 DTS](scripts/hdmi-probe/probe.dts) · [构建日志](artifacts/arm64-7.3.6-hdmi-probe/build.log)

COM4 按 115200 8N1 单进程独占。已验收的原厂 BL31 副本和测试镜像均由 TFTP 装入 RAM，分别回读 CRC32 `fa236592` 和 `f7ad6c38`。`boot.img` 从 `0x01fdffc0` 启动，共 19,626,056 字节，SHA256 为 `aa15e1d6f5fa112e9aace5748e8f08be5d0f59b07bc14a6ba43790e01d7cdab1`。Linux 根为内置 `rootfs`，`/proc/partitions` 无条目。U-Boot 网络环境只在 RAM 中设置。全程未写 U 盘 `/boot/boot.img` 或 U 盘根分区，未刷写 eMMC、访问 OTP 或执行 `saveenv`。 [镜像](artifacts/arm64-7.3.6-hdmi-probe/boot.img) · [SHA256 清单](artifacts/arm64-7.3.6-hdmi-probe/SHA256SUMS) · [验收脚本](scripts/hdmi-probe/board.py)

### 6.2 HPD 和 DDC

| 项目 | 板上读数 |
| --- | --- |
| HPD、RxSense | AON `+0x28=0x00000003`，HPD 和 RxSense 均为 1。直连采样无边沿。 |
| HPD 状态清理和 IRQ | AON `+0x34` 启动锁存 `0x00000003`，W1C 后为 0。AON `+0x30` 恢复 0。SPI 88 / INTID 120 在 Linux IRQ 19 注册，计数增量 0。热插拔边沿递送待验证。 |
| DDC 仲裁 | `REQ/ACK/ARB` 从 `0/0/0` 到 `1/1/0x45`，读完回到 `0/0/0`。TX 状态 `+0x48=0x20`（FIFO empty、busy 和错误位均清），AON `+0x54=0x20` 的低两位为 0，`+0x5c=0x0f`（SCL/SDA 均高）。 |
| 完整读取 | 256 字节。Block 0 为 EDID 1.3，Block 1 为 CTA-861 rev.3，累加和模 256 均为 0。对端声明 1 个扩展块，两块都在 segment 0，未执行跨段读。探针在 Block 2 起使用命令 4 和 7 位 segment pointer 地址 `0x30`（线上的写地址字节 `0x60`）。 |

[HPD 读数](artifacts/arm64-7.3.6-hdmi-probe/hpd.txt) · [DDC 读数](artifacts/arm64-7.3.6-hdmi-probe/edid.txt) · [EDID 二进制](artifacts/arm64-7.3.6-hdmi-probe/edid.bin) · [EDID 十六进制](artifacts/arm64-7.3.6-hdmi-probe/edid.hex)

采集端识别为 `HJW` / `HDMI TO USB`，Model 8497，首选 `1920×1080p60`。EDID SHA256 为 `b2538071e7ee7a08a2da912419313326b955233d2204894227c984e6c11a060f`。

| EDID 宣告项 | 解析结果 |
| --- | --- |
| SD 和桌面模式 | 720×480p59.94、720×576p50。640×480、800×600、1024×768、1280×768、1280×1024、1440×900、1600×900、1680×1050 约 60 Hz。 |
| HD 和 FHD | 1280×720p25/30/50/100/120/144，1920×1080p24/25/30/50/60/100/120。 |
| UHD/DCI 与 YCbCr 4:2:0 | 3840×2160p24/25/30，4096×2160p30。两种 4K 尺寸的 50/60 Hz 通过 YCbCr 4:2:0 Video Data Block 宣告为 420-only（VIC 96/97/101/102）。CTA 还宣告 4:4:4 和 4:2:2，HDMI VSDB 的最大 TMDS 字符率为 300 MHz。 |
| 10-bit 和 HDR10 | HDMI VSDB 未宣告 30-bit deep color，无 420 deep-color 宣告。无 HDR Static Metadata Data Block，也无 PQ 或 HDR10 静态元数据能力宣告。 |
| 音频 | LPCM 2 声道 FL/FR，32/44.1/48 kHz，16/20/24 bit。 |

`edid-decode --check` 同时报告对端字段符合性失败，涉及未使用 Standard Timing 项的填充值、物理尺寸不一致及缺少对应的 CTA VIC 等。两块均通过校验和及串口导出 SHA256 验证。 [完整解码及符合性报告](artifacts/arm64-7.3.6-hdmi-probe/edid-decode.txt) · [解析器说明](https://man.archlinux.org/man/extra/v4l-utils/edid-decode.1.en)

### 6.3 CEC 发送、SPI 147 和恢复

CRG `0xf8000048=0x60691161`，初始 CEC gate bit 19 已置位，reset bit 20 已清。CEC 初始 `+0x00=0xa08014cc`、`+0x04=0`、`+0x08=0x10020200`（bus-owner 字段 0）、`+0x0c=0`、`+0x14=0x0000f823`。当前连接无法量测 pin 13 初始电平，片内 bus-owner 字段只提供控制器状态。 [CEC 寄存器快照与发帧](artifacts/arm64-7.3.6-hdmi-probe/cec.txt)

| 测试帧 | 控制器结果 | INTID 179 增量 |
| --- | --- | --- |
| Polling `4→4` | NACK，AON bit 6 | 1 |
| Polling `4→0` | NACK，AON bit 6 | 1 |
| `4→0 <Give Physical Address>`（`0x83`） | TX_FAIL，AON bit 6，无 ACK、无 RX 回复 | 1 |

SPI 147 / INTID 179 在 Linux IRQ 20 注册，`/proc/interrupts` 总计增加 3。bit 6 的原厂 Polling 路径按 NACK 处理，普通帧的同位只记为发送失败，仲裁错误未单独辨识。收尾清测试 FIFO 和 AON 事件，回读控制字、地址、capture 和 opcode 恢复原值，AON `+0x30/34` 归零，CRG 与原值一致。CEC `+0x04` 记录了发送后的 `0x00000102`，其字段及清除语义待确认。 [IRQ 与状态恢复](artifacts/arm64-7.3.6-hdmi-probe/board-com4.txt)

**板级结论。** 已证实片内发送结果可经 INTID 179 递送。当前采集卡连接只有 NACK/失败，pin 13 连通性未定，暂缓接入 Linux `cec` 框架。接入前需要已知支持 CEC 的对端给出 ACK 或回复，或用分析仪量测该插座的 CEC 信号。 [Linux CEC 传输接口](https://docs.kernel.org/userspace-api/media/cec/cec-ioc-receive.html)

### 6.4 HDMI、AIAO 和 SMMU 并行回归

VDP 沿用固件 HD0 3840×2160、总计 4400×2250。静置时 DRM 会按空闲策略停用 vblank IRQ，验收通过现有 `WAIT_VBLANK` 请求持续观测 VT1，完成 1,080 次等待。四次采样的 VT1 计数为 371 → 519 → 635 → 1,191，三个探针命令执行期间每秒约增加 60 次。VT1 阈值回读始终为 `0x87b9800c`。VDP underflow、fetch underflow 和 SMMU INTID 181 均为 0。 [串口计数与时序](artifacts/arm64-7.3.6-hdmi-probe/board-com4.txt#L979)

测试期间 G0 显示静态彩条，暂时关闭 fbcon 光标闪烁并在采集后恢复。USB 以 1920×1080/30 FPS 采集，539/539 帧的 24 处彩条采样点全部匹配，黑帧 0，帧间最大间隔 34 ms。USB 采集开始 250 ms 后所有帧的缩小 RGB 图一致，未观察到黑屏或闪烁。AIAO 输出 12 秒 1 kHz 立体声 PCM，ALSA 播放 576,000 帧、48 kHz、零 XRUN，INTID 116 增加 1,200。采集的双通道音调均为 1 kHz，稳态 5,787 个 2 ms 窗口未出现静音。 [彩条截图](artifacts/arm64-7.3.6-hdmi-probe/hdmi-color-bars.png) · [18 秒 HDMI/PCM 采集](artifacts/arm64-7.3.6-hdmi-probe/hdmi-capture.mkv) · [采集分析](artifacts/arm64-7.3.6-hdmi-probe/capture-analysis.txt)

## 7. 2026-11-28 · 1080p60 ↔ 2160p30 RGB8 SDR 纯 RAM 模式切换

### 7.1 探针与操作边界

独立测试 DT 让 `scripts/hdmi-modeset/` 的探针独占 VDP/DHD0。开机继承 U-Boot 档位，读 `/sys/kernel/debug/hi3798_hdmi_modeset` 查看状态与寄存器，写入 `1080p60` 或 `2160p30` 才切换。G0 使用两个按目标尺寸预填的 DMA 彩条画布，GP0 设为 1:1。临时 AIAO 补丁复用 AIAO 已映射的 TX 音频和包寄存器，通过 ALSA PCM 打开锁串行化切档。播放设备打开期间返回 `EBUSY`，切档失败时保持静音。补丁仅应用于远端干净构建副本。 [临时驱动](scripts/hdmi-modeset/modeset.c) · [测试 DT](scripts/hdmi-modeset/modeset.dts) · [AIAO 补丁](scripts/hdmi-modeset/aiao.patch)

探针先静音并发送 AVMUTE，停音频和 AVI 包，关 PHY 通道。在旧档消隐区停 DHD0 并等待更新锁存，保留旧 G0 直到停扫。然后按第 2 节的表项设置 HPLL、VDP 分频、TX 像素门控、PHY TMDS 门控与复位。PHY 使用原厂 MV320 EC 驱动表 `c177e588/c177e5f4` 和 1/10 图样，并在打开通道时恢复原厂端接模式，CLK 用模式 2，D0–D2 用模式 3。DHD0 恢复后，TX 核对活动区、总计、像素时钟、TMDS 时钟和稳定状态，再打开 PHY 通道。音频用 48 kHz 对应的 N/CTS 重配 ACR，连续读取 HW CTS 后清 AVMUTE，后续 PCM trigger 按现有路径解静音。

彩条路径旁路 VDP GDM 和 TX CSC，G0 limited RGB 直接送到 TX。Video `+0x1a8[8:0]=0x11` 将 VDP R/G/B 路由成 TX 输入的 G/B/R 通道顺序。TX 设为 HDMI、8 bpc，关闭 420 分频、HDR GEN 槽位和本地加扰。AVI payload 为 RGB、BT.709、16:9、underscan 和 limited range，VIC 16 与 95 的校验字节分别为 `0xb9` 和 `0x6a`。TX2 MCLK/BCLK 沿用 12.288 MHz / 3.072 MHz，切档中复位窗口限于 TX Audio FIFO/ACR/Audio。 [原厂 AVI 槽位](#32-avi-与-hdr-静态包) · [Linux AVI 封包](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/drivers/video/hdmi.c)

COM4 115200 8N1 由单进程独占。经 TFTP 将已校验 BL31 副本载入 `0x08020000`，将测试镜像载入 `0x01fdffc0` 并执行 `bootm`。U-Boot TFTP 变量只设置在 RAM。内核配置禁用 MMC、MTD、USB Mass Storage 和 UAS，Linux 运行在内置 `rootfs`，`/proc/partitions` 无设备。U 盘、eMMC、OTP 及持久 U-Boot 环境均零写入。 [远端构建脚本](scripts/hdmi-modeset/build.sh) · [COM4 验收脚本](scripts/hdmi-modeset/board.py)

### 7.2 构建与实机结果

远端 `hedc@ArchLinux-E5-Server` 在 `/home/hedc/hg680kd-mainline` 从空输出目录使用 GCC 14.2.0 构建，编译和 DTB 打包均为零警告。`boot.img` 为 19,626,056 字节，SHA256 为 `1bc21a6017aff649b2ac87b33bd36ba49d3e4169c916b714003ac06db8db742f`。U-Boot 对 RAM `0x01fdffc0–0x03297807` 回读 CRC32 为 `22f0ef0c`，BL31 为 `fa236592`。 [镜像](artifacts/arm64-7.3.6-hdmi-modeset/boot.img) · [构建](artifacts/arm64-7.3.6-hdmi-modeset/build.log) · [校验清单](artifacts/arm64-7.3.6-hdmi-modeset/SHA256SUMS) · [COM4 日志](artifacts/arm64-7.3.6-hdmi-modeset/board-com4.txt)

U-Boot 初始档为 VIC 97、3840×2160p60 YCbCr 4:2:0 8 bpc，TMDS 297 MHz。Linux 继承初始 DHD0、PHY 与 AVI。实机经 debugfs 依次切入 1080p60、2160p30、1080p60。VDP VT1 的频率由首尾中断时间戳和计数差计算，TX `Video+0xc0/c4` 检查活动区与总计，`+0xc8/cc` 检查像素/TMDS 时钟。CRG `+0x150` 的低 7 位均为 1，TX `Packet+0x04[0]` 均为 1，采集端均恢复有效图像。第 8 节补齐 HPLL 与 PHY 的独立掉锁试验。本轮以 CRG 状态字、TX 测频、稳定标志和采集画面共同检验时钟链。 [U-Boot 寄存器](artifacts/arm64-7.3.6-hdmi-modeset/uboot-initial.txt) · [Linux 继承态](artifacts/arm64-7.3.6-hdmi-modeset/firmware-snapshot.txt)

采集卡 `345f:2131` 的 DirectShow 流固定为 1920×1080、30 FPS MJPEG，并同时录制 48 kHz 立体声 PCM。实测该设备还能把 1080p HDMI 输入输出成 3840×2160 的 UVC 流，故 UVC 流尺寸和帧率只用于描述采集设置。本节的 HID 采样覆盖接收状态。原生时序、位深和 EOTF 的独立回读见第 8 节。下表的线端目标时序由板端 TX、DHD0 与 VT1 实测，RGB8 SDR 由 TX 打包、AVI/HDR 槽位和采集画面交叉核对。 [UVC 缩放对照](artifacts/arm64-7.3.6-hdmi-modeset/first/1080p60-upscale-control.mkv) · [采集卡只读 HID 协议](https://raw.githubusercontent.com/steve-m/hsdaoh/master/src/libhsdaoh.c)

| 切换档位 | TX 活动区 / 总计 | 像素/TMDS MHz | VT1 Hz | AVI VIC | N / SW CTS | 播放时 5 次 HW CTS | 彩条帧 | XRUN |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1080p60 首次 | 1920×1080 / 2200×1125 | 148.500 / 148.500 | 60.000008 | 16 | 6144 / 148500 | 148500 / 148500 / 148500 / 148500 / 148500 | 353/353 | 0 |
| 2160p30 | 3840×2160 / 4400×2250 | 297.000 / 297.000 | 30.000003 | 95 | 6144 / 297000 | 297000 / 297000 / 297000 / 297001 / 297000 | 353/353 | 0 |
| 1080p60 回切 | 1920×1080 / 2200×1125 | 148.500 / 148.500 | 60.000002 | 16 | 6144 / 148500 | 148500 / 148500 / 148500 / 148500 / 148501 | 353/353 | 0 |

三档采集的每帧均检查八条色带的三个高度，切档后的采集段未见黑帧、花屏或通道错序。每档 AIAO 正常播放 384,000 帧，双声道 FFT 峰值均为 1,000 Hz，稳态无静音窗口。VDP underflow、fetch underflow 和 SMMU INTID 181 的累计值全程为零。 [机器可读结果](logs/541-hdmi-modeset-result.json) · [1080p60 录像](artifacts/arm64-7.3.6-hdmi-modeset/1080p60-first.mkv) · [2160p30 录像](artifacts/arm64-7.3.6-hdmi-modeset/2160p30.mkv) · [1080p60 回切录像](artifacts/arm64-7.3.6-hdmi-modeset/1080p60-return.mkv)

另在两次切档前后连续保持 USB 音频采集，三个档位各播放 144,000 帧，XRUN 均为 0。两个切档静音间隙的采样峰值为 37 和 398，16 位 PCM 满幅为 32768。两个峰值均低于测试的 500 计数阈值。 [连续录音](artifacts/arm64-7.3.6-hdmi-modeset/transition-audio.wav) · [静音间隙分析](logs/541-hdmi-transition-audio.json) · [收尾 IRQ 和挂载](artifacts/arm64-7.3.6-hdmi-modeset/final.txt)

### 7.3 关键寄存器回读

| 寄存器 | 1080p60 首次及回切 | 2160p30 |
| --- | --- | --- |
| CRG HPLL `+0x28/2c` | `11800000 / 00001031` | `12000000 / 00001063` |
| CRG VDP 分频 `+0xd8/34c` | `40010019 / 01104000` | `400d0019 / 01104000` |
| CRG TX/PHY `+0x10c/110` | `00012437 / 00000041` | `00012437 / 00000001` |
| CRG 状态 / TX 稳定 `+0x150 / Packet+0x04` | `0000007f / 00000001` | `0000007f / 00000001` |
| PHY 1/10 图样 `+0x50/54` | `00003fa0 / 01f07c1f` | `00003fa0 / 01f07c1f` |
| PHY 通道、端接、分频和驱动 `+0x64/68` | `002009f0 / 0075d75d` | `000009f0 / 0079e79f` |
| PHY PLL/SSC `+0x8c/90/b0/b4/b8/bc` | `000246a0 / 00000000 / 01640000 / 01c01001 / 00000000 / 00000000` | `000246a0 / 00000000 / 01640000 / 01c01001 / 00000000 / 00000000` |
| VDP 时序与 VT1 `+0xc208/210/220/228` | `00280437 / 00bf077f / 0004002b / 87b98444` | `0051086f / 017f0eff / 00090057 / 87b9887c` |
| AVI `Packet+0x18/1c/20` | `000d0282 / 04a002b9 / 00000010` | `000d0282 / 04a0026a / 0000005f` |
| AIAO TX2 接口 `+0x00` | `e4280014` | `e4280014` |

完整快照含 PHY 模拟驱动、DHD0 活动区、G0 锁存地址、TX 包与 TX2 DMA 状态。 [1080p60 首次](artifacts/arm64-7.3.6-hdmi-modeset/1080p60-first-snapshot.txt) · [2160p30](artifacts/arm64-7.3.6-hdmi-modeset/2160p30-snapshot.txt) · [1080p60 回切](artifacts/arm64-7.3.6-hdmi-modeset/1080p60-return-snapshot.txt)

## 8. 2026-11-28 · HPLL / PHY 掉锁试验与 HDMI RX 回读

### 8.1 HPLL 与 PHY 锁定状态

同族海思 HDMI 头文件把 `CRG+0x2c[20]` 定义为 `hpll_pd`，并区分 PHY `+0x60[2]` 的 `ad_txpll_lock` 与 `+0x98[20]` 的 `hdmi_clk_lock`。本板 RAM 探针在 AVMUTE、PHY 输出关断和 DHD0 停扫后，仅翻转目标 PLL 的掉电位。试验前后的控制字按原掩码恢复，随后运行正常切档。 [海思 CRG 定义](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC060/blob/a82e5fee18a794ecdf28401b21c25cddec0d21fb/source/msp/drv/hdmi/hdmi_2_0/product/hisiv100/regs/hdmi_reg_crg.h#L150-L173) · [海思 PHY 定义](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC060/blob/a82e5fee18a794ecdf28401b21c25cddec0d21fb/source/msp/drv/hdmi/hdmi_2_0/hal/phy/hisiv100/regs/hdmi_reg_dphy.h#L598-L639) · [测试补丁](scripts/hdmi-modeset/locks.patch)

- **HPLL 锁定位是 `0xf8a22150[3]`。** PHY 保持隔离时，单独置位 `0xf8a2202c[20]`，CRG 锁字由 `0x7f` 变为 `0x77`。复原后回到 `0x7f`。三次切换均只翻转 bit 3。恢复旧参数后首次读回锁定所需约 37–44 µs，该时间包含软件轮询。
- **PHY 可用的锁定位是 `0xf8d30098[20]`。** 单独置位 `0xf8d30064[20]` 给 PHY TXPLL 掉电时，CRG 锁字保持 `0x7f`，PHY 状态 bit 20 由 1 变 0，恢复 PHY 后回到 1。同族头文件标注的 `0xf8d30060[2]` 在本板三态均为 0，`+0x60` 实读为 `0x1`。TX `Packet+0x04[0]` 继续作为 TX 内部同步状态。 [三档锁位与接收端原始数据](artifacts/arm64-7.3.6-hdmi-modeset-locks/locks-rx.json) · [负对照串口记录](artifacts/arm64-7.3.6-hdmi-modeset-locks/locks-rx-com4.txt)

`modeset-locks` 的探针镜像用 GCC 14.2.0 从空目录构建。SHA256 为 `68153ee12b11c259cd0133a015b5fe0d7da88143856a870214d42269b5b8f7f9`，U-Boot RAM 回读 CRC32 为 `b8776a37`。全段使用 COM4 TFTP，Linux 根为内置 `rootfs`，无块设备分区和挂载。VDP underflow、fetch underflow、SMMU INTID 181 和 PCM XRUN 均为 0。 [镜像](artifacts/arm64-7.3.6-hdmi-modeset-locks/boot.img) · [启动与回归](artifacts/arm64-7.3.6-hdmi-modeset-locks/board-com4.txt)

### 8.2 MacroSilicon MS2131 原生时序

通过采集卡 `345f:2131` 的 HID `B5` 读取 HDMI RX 的 XDATA。原始 `E184/E18C` 为活动区，`E184–E195` 同时包含前肩、消隐和同步宽度，`E14E/E170` 的总计与活动区加消隐互相核对。AVI 在 `E3A0` 起回读，两档包版本、长度及校验和通过。接收端 `E390/E394` 保存实际收到的 ACR CTS/N，以测试流 48 kHz 为参考按 `Fchar=128×Fs×CTS/N` 计算 TMDS 字符率，再按 RGB 24 bit 时的字符率与像素率等值及接收端总计计算刷新率。`E160–E162` 的原始字段随两档字符率约成 1∶2，换算系数尚未校准。 [只读回读实现](scripts/hdmi-modeset/receiver.py) · [MS213x 活动区线索](https://github.com/BertoldVdb/ms-tools/blob/main/ms213x_patch/asm/readinfo2.asm) · [接收端原始读数](artifacts/arm64-7.3.6-hdmi-modeset-locks/locks-rx.json)

| 切换档位 | HDMI RX 活动区 / 总计 | 水平前肩 / 同步 / 后肩 | 垂直前肩 / 同步 / 后肩 | RX AVI VIC / 格式 | RX ACR N / CTS | ACR 推算刷新率 |
| --- | --- | --- | --- | --- | --- | --- |
| 1080p60 首次 | 1920×1080 / 2200×1125 | 88 / 44 / 148 | 4 / 5 / 36 | 16 / RGB | 6144 / 148500 | 60.000 Hz |
| 2160p30 | 3840×2160 / 4400×2250 | 176 / 88 / 296 | 8 / 10 / 72 | 95 / RGB | 6144 / 297000 | 30.000 Hz |
| 1080p60 回切 | 1920×1080 / 2200×1125 | 88 / 44 / 148 | 4 / 5 / 36 | 16 / RGB | 6144 / 148500 | 60.000 Hz |

固件 VIC 97 的 4:2:0 对照在同一 MS2131 上读到 1920×2160 / 2200×2250，横向计数相当于 RGB 档的半宽口径。`E160` 在该对照中约为 399995，和 RGB 1080p60 接近。原始计数的时钟域有待进一步辨明，表中 Hz 来自 RX ACR 和原生总计。 [固件对照](artifacts/arm64-7.3.6-hdmi-modeset-rx-proof/receiver-initial.json)

### 8.3 线端 GCP 位深与 SDR EOTF

用纯 RAM 的 `modeset-rxtag` 镜像手动开关 24-bit GCP 标记和 SDR DRM InfoFrame。HDMI RX `E380[7:4]` 的 CD=4 表示每通道 8 bit。`E4C0/E4C1` 保存 DRM 版本 1 和长度 26，`E4C4` 为校验和，`E4C5[2:0]` 的 EOTF=0 表示传统 Gamma SDR。三档独立回读完整包时校验和均有效。 [GCP 位深编码](https://docs.altera.com/r/docs/823533/26.1/gts-hdmi-ip-user-guide/source-general-control-packet-gcp) · [Linux DRM EOTF 枚举](https://raw.githubusercontent.com/gregkh/linux/v7.3.6/include/linux/hdmi.h) · [测试补丁](scripts/hdmi-modeset/receiver-tags.patch)

RX 可在停止发送后保留上次的 GCP 和 DRM。每档先采样接收缓存，然后在 SDR DRM 中放入与缓存不同的 MaxCLL 值作为新包标记，再要求 RX 回读该标记。标记仅用于确认包的新鲜度。发包测试不改变 RGB 像素打包和原生时序。最初的对照还在两档都测到开启位深标记时 `E380` 由 `0x01` 变 `0x41`。 [1080p60 初次原始包对照](artifacts/arm64-7.3.6-hdmi-modeset-rxtag/1080p60-first-tags-on-rx-e300.bin) · [2160p30 初次原始包对照](artifacts/arm64-7.3.6-hdmi-modeset-rxtag/2160p30-tags-on-rx-e300.bin)

| 档位 | RX E380 | CD / 位深 | RX DRM EOTF | 上次 → 新 MaxCLL 标记 | DRM 校验字节 |
| --- | --- | --- | --- | --- | --- |
| 1080p60 首次 | `0x41` | 4 / 8 bpc | 0 / SDR | 116 → 216 | `0x86` |
| 2160p30 | `0x41` | 4 / 8 bpc | 0 / SDR | 空 → 195 | `0x9b` |
| 1080p60 回切 | `0x41` | 4 / 8 bpc | 0 / SDR | 195 → 116 | `0xea` |

两种发包标记在测试后关闭，板子停留在 1080p60 RAM 档。回读 `Packet+0x208=0x101`、`+0x1b4=0`。此镜像在空目录以 GCC 14.2.0 零警告构建，SHA256 为 `22219e855a81d1f2ec6c5e62281e9ce2847382cae65ee62103808601a0a2fd1c`，U-Boot RAM CRC32 为 `d5f55954`。本轮 VDP underflow、fetch underflow 和 SMMU INTID 181 均为 0。U 盘、eMMC、OTP 和持久 U-Boot 环境均零写入。 [最终镜像](artifacts/arm64-7.3.6-hdmi-modeset-rx-proof/boot.img) · [COM4 日志](artifacts/arm64-7.3.6-hdmi-modeset-rx-proof/locks-rx-com4.txt) · [三档完整回读](logs/541-hdmi-locks-rx-result.json) · [收尾寄存器与挂载](artifacts/arm64-7.3.6-hdmi-modeset-rx-proof/locks-rx-final.txt)

## 9. 2026-11-28 · 2160p60 YCbCr 4:2:0 8 bpc 纯 RAM 往返切换

### 9.1 视频与时钟路径

`4k60` 构建变体沿用第 8 节的临时探针。切档轮询 `0xf8a22150[3]` 和 `0xf8d30098[20]`。VIC 97 的 DHD0 使用 3840×2160 活动区和 4400×2250 总计，像素时钟为 594 MHz。2160p60 彩条沿用固件的 GP0 二倍缩放，G0 读取 1920×1080 ARGB，GP0 输出完整 UHD 光栅。首轮 3840×2160 ARGB 直读触发了 VDP underflow。 [临时探针补丁](scripts/hdmi-modeset/4k60.patch) · [原厂 GP0 回读](artifacts/arm64-7.3.6-hdmi-modeset/uboot-extra.txt) · [全宽 ARGB 对照](artifacts/arm64-7.3.6-hdmi-4k60/preflight-full-raster/trial-2160p60-switch.txt)

4:2:0 档启用固件预置的 VDP GDM RGB→YCbCr CSC（`VDP+0xd500[0]`）。TX CSC 旁路，TX Video `+0x180=0x7`、`+0x18c=0x401`、`+0x1a8=0x88` 选择 Y/Cb/Cr 输入、横纵向采样及二分频。RGB 档关闭 GDM，恢复 `+0x180=0x8`、`+0x18c=0x0`、`+0x1a8=0x11`。两条通路均为 8 bpc。 [原厂 TX Video 回读](artifacts/arm64-7.3.6-hdmi-modeset/uboot-initial.txt) · [只用 TX CSC 的偏色对照](artifacts/arm64-7.3.6-hdmi-4k60/preflight-tx-csc/trial-2160p60.png)

### 9.2 EDID 与 SCDC

现场 EDID 给出 300 MHz TMDS 上限、VIC 97 仅限 YCbCr 4:2:0，且无 HDMI Forum SCDC 或 4:2:0 deep-color 声明。MS2131 仍会应答 `0x54`，`Sink_Version (0x01)` 返回 `0x00`。低速切档经 DDC 读改写 `TMDS_Config (0x20)[1:0]` 为零，回读 `0x20`、`Scrambler_Status (0x21)` 与 `Status_Flags_0 (0x40)`。TX `Packet+0x260` 清加扰与 HDMI2 encoder 位，PHY 使用 `0x1f / 0x01f07c1f` 的 1/10 图样。 [EDID 解码](artifacts/arm64-7.3.6-hdmi-probe/edid-decode.txt) · [DDC/SCDC 实现](scripts/hdmi-modeset/scdc.c)

探针另有受 EDID 和 SCDC 版本约束的 594 MHz RGB8 分支。满足能力时按同一模式提交启用 `TMDS_Config[1:0]`、TX 加扰和 PHY 1/40，回读加扰与锁定位。当前 MS2131 对该档返回 `-EOPNOTSUPP`，切入前的 AVI 保持不变。1/40 的线端验证需要另一台声明对应模式、TMDS 带宽和 SCDC 的接收端。 [594 MHz 拒绝回读](artifacts/arm64-7.3.6-hdmi-4k60/reject-2160p60-rgb.txt)

### 9.3 构建与启动

`hedc@ArchLinux-E5-Server` 在 `/home/hedc/hg680kd-mainline` 使用 GCC 14.2.0 从空输出目录构建，内核及 DTB 打包日志均为零警告。最终 `boot.img` 为 19,626,056 字节，SHA256 为 `3103f93e4c83fb8b0d2eb0686970e6c225a3a227426db94b7595d94f6d9c1f69`。COM4 115200 8N1 单进程独占，TFTP 载入 BL31 与镜像，U-Boot RAM 回读 CRC32 分别为 `fa236592` 和 `e9f95936`。镜像在 `0x01fdffc0` 启动，Linux 使用内置 `rootfs`。 [镜像](artifacts/arm64-7.3.6-hdmi-4k60/boot.img) · [校验清单](artifacts/arm64-7.3.6-hdmi-4k60/SHA256SUMS) · [远端构建日志](artifacts/arm64-7.3.6-hdmi-4k60/build.log) · [COM4 日志](artifacts/arm64-7.3.6-hdmi-4k60/4k60-com4.txt)

### 9.4 五次原生切档实测

MS2131 通过 HID 独立读出 HDMI RX 的活动区、总计、AVI、GCP、SDR DRM 和 ACR。4:2:0 档 RX 横向按 TMDS 字符计数，一个字符覆盖两个水平像素，原始 1920／2200 对应像素 3840／4400。TX Video 内部时序检测和 DHD0 均回读完整光栅。以 48 kHz PCM 为参考，RX 刷新率按 `(128 × 48000 × CTS / N) / (RX 字符总计 × Vtotal)` 计算。30 Hz 和两次 60 Hz UHD 的接收端 ACR 均为 `6144 / 297000`。 [验收脚本](scripts/hdmi-modeset/accept_4k60.py) · [逐档原始 RX 与 TX 读数](logs/542-hdmi-4k60-result.json)

| 顺序 | MS2131 原始活动区 / 总计 | AVI VIC / Y / 校验字节 | 像素 / TMDS MHz | RX ACR Hz | VDP VT1 Hz | N / SW CTS / 播放中 HW CTS | 通过彩条帧 |
| --- | --- | --- | --- | ---: | ---: | --- | ---: |
| 1080p60 首次 | 1920×1080 / 2200×1125 | 16 / 0 / `b9` | 148.500 / 148.500 | 60.000 | 59.999967 | 6144 / 148500 / 148500 | 354/354 |
| 2160p30 | 3840×2160 / 4400×2250 | 95 / 0 / `6a` | 297.000 / 297.000 | 30.000 | 30.000004 | 6144 / 297000 / 297000–297001 | 353/353 |
| 2160p60 首次 | 1920×2160 / 2200×2250 | 97 / 3 / `0c` | 594.000 / 297.000 | 60.000 | 59.999998 | 6144 / 297000 / 297000 | 353/353 |
| 回切 1080p60 | 1920×1080 / 2200×1125 | 16 / 0 / `b9` | 148.500 / 148.500 | 60.000 | 60.000008 | 6144 / 148500 / 148500 | 354/354 |
| 再切 2160p60 | 1920×2160 / 2200×2250 | 97 / 3 / `0c` | 594.000 / 297.000 | 60.000 | 60.000007 | 6144 / 297000 / 297000 | 353/353 |

- 五次切档的 RX AVI 都更新且校验和有效。GCP `E380=0x41` 给出 CD=4（8 bpc）。RX SDR DRM 的 EOTF 为 0，MaxCLL 测试标记依次为 116、195、197、116、197，每次均区别于该档发包前的接收缓存。测试标记在各档取证后关闭，最终回读 `Packet+0x208=0x101`、`+0x1b4=0`。 [首次 4K60 包回读](artifacts/arm64-7.3.6-hdmi-4k60/2160p60-first-rx-tags.json) · [再次切入后的包回读](artifacts/arm64-7.3.6-hdmi-4k60/2160p60-return-rx-tags.json)
- 五次切档的 SCDC `0x20` 写前和回读均为 `0x00`，`0x21=0x00`。第一次切入 2160p60 时 `0x40` 从 `0x01` 收敛至 `0x0f`，其余四次切档已读到 `0x0f`。五档稳态均为 `0x0f`，DDC 请求、应答与仲裁归零，TX `Packet+0x260=0x00`，PHY 保持 1/10。 [2160p60 切入](artifacts/arm64-7.3.6-hdmi-4k60/2160p60-first-switch.txt) · [2160p60 稳态](artifacts/arm64-7.3.6-hdmi-4k60/2160p60-first-scdc-stable.txt) · [1080p60 回切稳态](artifacts/arm64-7.3.6-hdmi-4k60/1080p60-return-scdc-stable.txt)
- 每档稳定后以 1920×1080 MJPEG 30 FPS 采集，共核验 1,767 帧，彩条未见偏色、撕裂或黑帧。每档 AIAO 播放 384,000 帧、48 kHz 立体声 PCM，采集双声道均测得 1 kHz，稳态静音窗口及 ALSA XRUN 均为 0。 [2160p60 彩条](artifacts/arm64-7.3.6-hdmi-4k60/2160p60-first.png) · [2160p60 再切录制](artifacts/arm64-7.3.6-hdmi-4k60/2160p60-return.mkv) · [1080p60 回切录制](artifacts/arm64-7.3.6-hdmi-4k60/1080p60-return.mkv)
- HPLL 每档隔离回读 `0x7f → 0x77 → 0x7f`，PHY TXPLL bit 20 每档为 `1 → 0 → 1`。最终验收整轮的 VDP underflow、fetch underflow 和 SMMU INTID 181 都为 0。板子停在 2160p60 RAM 档，`/proc/partitions` 无设备。U 盘 `/boot/boot.img`、根分区和 eMMC 的写入次数为 0。U-Boot 命令限于易失网络变量设置、TFTP、RAM 校验和引导，`saveenv` 和 OTP 访问次数均为 0。 [COM4 完整日志](artifacts/arm64-7.3.6-hdmi-4k60/4k60-com4.txt) · [最终 IRQ、寄存器与挂载](artifacts/arm64-7.3.6-hdmi-4k60/final.txt)
