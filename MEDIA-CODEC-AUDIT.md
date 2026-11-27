# HG680KD 多媒体硬件资源摸底

2026-11-28 · 烽火 HG680KD / Hi3798MV320 · Linux 7.3.6

JPGD 和 VENC 在 Linux `7.3.6-hg680kd-arm64-codec-probe` 纯 RAM 诊断镜像上通过 MMIO 写回读及 CRG 恢复校验。JPGE 候选 `CRG +0x090[0]` 写 1 后读回 0，探针在访问候选引擎 MMIO 前停止。JPGD 另有同板原厂 U-Boot 完成 1280×720 Logo 硬解码的证据。 [原厂串口记录](logs/108-reboot-usb-phy.txt#L89) · [RAM 诊断 dmesg](artifacts/arm64-7.3.6-codec-probe/board.txt#L603)

## 1. 硬件资源总表

| 模块 | 物理 MMIO 范围、长度 | GICv3 SPI / INTID | CRG `0xf8a22000` | DMA / SMMU | 板端判定 |
| --- | --- | --- | --- | --- | --- |
| JPGD | 核心 `0xf8c40000–0xf8c406ef`（`0x6f0`），SMMU `0xf8c4f000–0xf8c4f363`（`0x364`）。均为原厂映射长度。 | `97` / `129`，高电平 | 核心 `+0x07c`，SMMU `+0x334`，各 bit 0 开时钟、bit 4 置位复位。此版驱动未设置频率选择 | 私有 SMMU。原厂走页表，物理/CMA 方案取决于旁路验证。 | **确认存活**。7.3.6 MMIO 和 CRG 验活通过，原厂固件已有实机硬解码记录。 |
| JPGE | `0xf8c90000–0xf8c9ffff`（`0x10000`），Hi3798M V200 地址图。该 SDK 驱动映射前 `0x10c0`。均为 MV320 候选。 | `102` / `134`，高电平 | 候选 `+0x090` bit 0 开时钟、bit 4 置位复位。相邻型号定义无选频字段 | 相邻型号 HAL 将 MMZ 物理地址写入引擎，SMMU 输入先由 TDE 搬到 MMZ。MV320 待确认。 | **候选 CRG 不匹配**。使能位写 1 读回 0，MMIO 及硬件屏蔽状态待确认。 |
| VENC / VEDU | `0xf8c80000–0xf8c8ffff`（`0x10000`）整窗。核心状态组在 `+0x3000`，SMMU 位于尾部 `0xf8c8f000–0xf8c8ffff`（`0x1000`）。 | `101` / `133`，高电平 | 核心 `+0x08c` bit 0/1 开核心/AXI 时钟、bit 4 置位复位、bit 8 原厂清零。SMMU `+0x32c` bit 0 开时钟、bit 4 置位复位 | 私有 SMMU。原厂为编码通道准备页表和错误地址，物理旁路待验证。 | **确认存活**。7.3.6 MMIO 和 CRG 验活通过，H.264 编码作业待验证。 |

原厂 GIC 节点为 `arm,gic-v3`，三个 IRQ 来自 `virtdev` 的同序 `interrupt-names` / `interrupts`。`jpeg`、`venc`、`jpge` 的元组分别为 `<0 0x61 4>`、`<0 0x65 4>`、`<0 0x66 4>`。设备树第二格是 SPI 索引，GIC INTID 要加 32。原厂 JPGD 和 VENC 的申请函数分别传入 `0x81`、`0x85`，`drv_osal_request_irq` 最终用名字查 Linux IRQ。该 DTB 通过 `virtdev` 集中描述中断，未提供三个引擎各自的 `reg` 或 `iommus` 属性。 [GIC 节点](artifacts/vpu-reverse-factory/out/factory.dts#L545) · [virtdev](artifacts/vpu-reverse-factory/out/factory.dts#L1135) · [原厂符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L21400)

## 2. 原厂驱动证据

`arm32-result.txt` 提供原厂 `drv_sys_reg_init`、`drv_sys_get_crg_reg_ptr`、`pdt_init` 等基线反编译，其中 `pdt_init` 属于 VDEC。用同一份 ARM32 原厂内核和 `kallsyms` 补反编译本轮的 JPGD 和 VENC 函数。下文的函数地址为原厂 ARM32 VA，物理地址来自映射参数和寄存器偏移。 [原反编译](artifacts/vpu-reverse-factory/arm32-result.txt) · [补充反编译](artifacts/media-codec-audit/stock-codecs-arm32.txt)

### JPGD

- **MMIO 与探针。** `JPEG_DRV_K_ModInit.part.9`（`0xc06bd1e0`）两次 `ioremap` 确定核心和 SMMU 的范围。`jpgd_reg_get_resume_state` 读核心 `+0x000[1]`，该字也含启动位。`DRV_JPEG_GetIntStatus` 读 `+0x100`，同字的写入用于清状态。`+0x104` 为中断控制，`+0x040` 为时钟门控/Outstanding/缩放配置，可读默认值。当前反编译只确认这些状态和配置字，独立版本 ID 与复位后的固定非零常量待查。 [映射及 IRQ](artifacts/media-codec-audit/stock-codecs-arm32.txt#L1890) · [状态与清除](artifacts/media-codec-audit/stock-codecs-arm32.txt#L628) · [核心配置](artifacts/media-codec-audit/stock-codecs-arm32.txt#L3973)
- **CRG。** `DRV_JPEG_OnClock`（`0xc06be82c`）先置 `+0x334[0]`，再置 `+0x07c[0]`，含屏障和回读。`DRV_JPEG_CancelReset`（`0xc06bd640`）先清 `+0x334[4]`，再清 `+0x07c[4]`。`jpgd_hal_set_clock` 也独立证实两组 bit 0/4，可见时钟、复位写入后的屏障与延时。核心 `+0x040[3]` 属于引擎内部控制。 [时钟](artifacts/media-codec-audit/stock-codecs-arm32.txt#L3122) · [复位](artifacts/media-codec-audit/stock-codecs-arm32.txt#L2191) · [HAL 时序](artifacts/media-codec-audit/stock-codecs-arm32.txt#L3873)
- **SMMU。** 原厂清 `0xf8c4f000[0]`、置 bit 3，将 `drv_smmu_get_page_table_addr` 返回值写入 SMMU `+0x2e4/+0x2ec`（页表基址）和 `+0x304/+0x30c`（错误地址），分别保留最高位并核对非零。`jpgd_mem_alloc` 使用 `drv_smmu_alloc`。`jpgd_reg_set_mmu_bypass_chn` 在作业配置中向核心 `+0x134` 写零。函数名给出了控制用途，物理旁路的全局/通道位组合仍要独立确认。 [SMMU 初始化](artifacts/media-codec-audit/stock-codecs-arm32.txt#L2233) · [通道寄存器](artifacts/media-codec-audit/stock-codecs-arm32.txt#L4287) · [分配](artifacts/media-codec-audit/stock-codecs-arm32.txt#L4694)
- **能力与裁剪。** `jpgd_get_capbility` 返回编译期静态表 `g_drv_jpgd_capability`（`0xc1768370`）。原始数据含三组 `64, 64, 8192, 8192` 和 `+0x30 = 0x27`。输出高度、部分采样格式的输出高度、VU 交换由该表的 bit 1、2、5 判定。追到驱动注册及开钟、解复位路径，未见读取芯片能力熔丝来拒绝 JPGD。表内数值仍需结合格式和步幅作业实测。 [能力表符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L108003) · [能力访问函数](artifacts/media-codec-audit/stock-codecs-arm32.txt#L5261)

### JPGE

- **本板证据。** 原厂 DTB 有 `jpge` 中断。原厂内核仅找到名为 `JPGE_DRV_Suspend` / `JPGE_DRV_Resume` 的相关符号，它们调用 JPGD 的 `DRV_JPEG_*` 例程，使用 JPGD 的软件上下文。供应商模块清单里未见 `hi_jpegenc.ko`。现有原厂二进制样本缺少独立 JPGE HAL，因此初始化期的芯片能力/OTP 判断、独立 CRG 及 SMMU 路径都缺本板证据。 [Suspend](artifacts/media-codec-audit/stock-codecs-arm32.txt#L2442) · [Resume](artifacts/media-codec-audit/stock-codecs-arm32.txt#L3236) · [原厂模块清单](logs/404-vpu-factory-inventory.txt#L20)
- **相邻型号资料。** Hi3798M V200 地址图给出 `0xf8c90000` 的 64 KiB JPGE 窗口。SDK 的 `PERI_CRG36` 为 `+0x090`，bit 0 是 `jpge_cken`，bit 4 是 `jpge_srst_req`。HAL 映射 `sizeof(S_JPGE_REGS_TYPE)`（`0x10c0`），表中 `JPGE_VERSION` 在 `+0x0fc`、`INTSTAT` 在 `+0x000`、`RAWINT` 在 `+0x008`。前者是优先 ID 候选，具体非零版本值和对 MV320 的适用性待验。 [海思 Hi3798M V200 地址图](https://atta.szlcsc.com/upload/public/pdf/source/20220608/83C832A827A7FB455622F999A722688E.pdf) · [Hi3798MV200 CRG 定义](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/common/drv/include/hi3798mv200/hi_reg_crg.h#L567) · [JPGE 寄存器布局](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/msp/drv/jpge/src/jpge_reg.h#L528)
- **DMA 与能力候选。** 相邻 SDK 的 `JPEG_ENC_Open` 申请 IRQ 并映射寄存器，未查芯片能力状态。作业写源面和输出码流的物理地址，`CONFIG_GFX_MMU_SUPPORT` 下由 TDE 把 SMMU 输入复制到 MMZ，再用其物理地址编码。后续应先核实 MV320 的地址图和 JPGE HAL，再判断是否可以直接使用 32-bit CMA DMA。 [相邻型号 JPGE HAL](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/msp/drv/jpge/src/jpge_hal.c)

### VENC / VEDU

- **MMIO 与探针。** `drv_venc_hal_v500_r002_init`（`0xc09cbb48`）映射 `0xf8c80000`、`0x10000`。`drv_venc_hal_v500_r002_read_reg` 读核心 `+0x3000/+0x3004` 的事件状态，`clear_bus` 轮询 `+0x3010` 的总线状态。SMMU `+0x018/+0x028` 有中断状态，清除在 `+0x01c/+0x02c`。`read_reg_smmu` 含清中断写入，适合从中提取寄存器偏移。当前没有确定的独立硬件版本字。`V500 R002` 来自软件 HAL 选择。 [整窗映射](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24435) · [状态读取](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24285) · [总线状态](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24396)
- **CRG。** `drv_venc_pdt_open_hardware`（`0xc09cec34`）置 `+0x32c[0]`，对 `+0x08c` 做 `(old & ~0x103) | 0x3`。`reset_hardware`（`0xc09cec90`）先 SMMU、再核心，分别脉冲 bit 4（置位后等待 5 µs，清位后等待 10 µs）。相邻型号头文件将 `+0x08c[8]` 命名为 `venc_clk_sel`，MV320 原厂选择编码为 0，对应父时钟和 MHz 待确认。 [原厂启停和复位](artifacts/media-codec-audit/stock-codecs-arm32.txt#L27415) · [Hi3798MV200 选频定义](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/common/drv/include/hi3798mv200/hi_reg_crg.h#L547)
- **SMMU。** 创建编码通道时通过 `drv_venc_get_smmu_addr` 获取页表和错误地址。`venc_hal_cfg_smmu_reg` 写私有 SMMU 窗口 `+0x2ec/+0x304/+0x30c`。原厂有 SMMU 分配与 MMZ 分配接口。采用 MMZ/CMA 只解决缓冲区来源，真实 DMA 通路仍须验证。 [取表](artifacts/media-codec-audit/stock-codecs-arm32.txt#L5799) · [编码通道](artifacts/media-codec-audit/stock-codecs-arm32.txt#L14361) · [SMMU 配置](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25298)
- **能力与裁剪。** `venc_drv_efl_hal_global_init` 经 `drv_sys_get_chip_version` 读取 `SYSCTRL 0xf8000ee0` 和 `PERI 0xf8a20840[20:16]` 作 HAL 兼容性分支。历史同板硅 ID 为 `0x37980320`，本轮 `PERI` 字为 `0xc0100000`，字段为 `0x10`，组合推导返回类型 `0x10`、版本 `0x0320`，命中 V500 R002 初始化分支。`venc_drv_get_capability` 和原厂 Android XML 给出 H.264 `1920×1088` 的软件声明上限。所查初始化和打开路径未发现额外的 VENC OTP disable 判断。通用 `drv_sys_get_video_codec_support` 读取的是 `g_video_codec` 软件变量（文件初值 `0x7ff`），`drv_sys_disable_video_codec` 软件清位。 [芯片识别](artifacts/media-codec-audit/stock-codecs-arm32.txt#L171) · [HAL 版本分支](artifacts/media-codec-audit/stock-codecs-arm32.txt#L17487) · [编码能力](artifacts/media-codec-audit/stock-codecs-arm32.txt#L9018) · [原厂媒体配置](artifacts/vpu-reverse-factory/extracted/vendor/etc/media_codecs.xml#L194) · [历史硅 ID](logs/406-vpu-uboot-registers.txt#L4)

## 3. Linux 7.3.6 板端探测

### 3.1 原 USB 基线只读初值

COM4 单进程独占，115200 8N1，复用已运行的 `7.3.6-hg680kd-arm64-usb`。在 debugfs 的两个 `dummy-syscon` regmap 上，从相应行偏移各读 14 字节并检查寄存器行号。两处窗口分别为 CRG `0x000–0x3fc` 和 PERI `0x000–0x870`，读取限于下列六个 32-bit 字。读数是 Linux 会话的探测前初值，包含固件交接状态。 [接口与权限](artifacts/media-codec-audit/mainline-regmap.txt) · [原始串口](artifacts/media-codec-audit/mainline-snapshot.txt) · [内核 regmap debugfs 实现](https://github.com/torvalds/linux/blob/master/drivers/base/regmap/regmap-debugfs.c)

| 地址 | 读值 | 含义 |
| --- | --- | --- |
| `0xf8a2207c` | `0x00000010` | JPGD 核心时钟关闭，复位置位。 |
| `0xf8a22334` | `0x00000010` | JPGD SMMU 时钟关闭，复位置位。 |
| `0xf8a22090` | `0x00000000` | JPGE 候选 CRG 字。按相邻型号布局解读为时钟关闭、复位解除。 |
| `0xf8a2208c` | `0x00000003` | VENC 核心和 AXI 时钟位开启，复位解除，选择位为 0。 |
| `0xf8a2232c` | `0x00000001` | VENC SMMU 时钟位开启，复位解除。 |
| `0xf8a20840` | `0xc0100000` | `PERI[20:16] = 0x10`，用于原厂芯片版本选择。 |

本轮串口返回结束标记，`dmesg` 未匹配 SError 或外部中止。这个结果覆盖上述 CRG/PERI 读取。运行镜像无 `/dev/mem`，`CONFIG_DEVMEM`、`CONFIG_UIO`、`CONFIG_MODULES` 均关闭，regmap `registers` 为 `0400`，三个引擎没有已注册的驱动资源窗口。仓库已有的 7.3.6 VDP、AIAO、KVM RAM 镜像的内核配置也关闭了这些入口。历史日志显示原厂 U-Boot 重启路径会从 eMMC 加载环境。 [运行镜像与挂载状态](artifacts/media-codec-audit/mainline-preflight.txt) · [内核配置和资源](artifacts/media-codec-audit/mainline-access.txt) · [VDP RAM 配置](artifacts/arm64-7.3.6-vdp/kernel.config#L1451) · [原厂 U-Boot 启动记录](logs/108-reboot-usb-phy.txt#L59)

这次基线读取在初值采集后停止，保持了当时的 CRG 和引擎寄存器状态。

同板历史 U-Boot 日志在 `LOGO Flag` 后记录 `jpgd_dfx_print_timestamp ... hard in 1280 720 out 1280 720 ... dec 4`，证明该机 JPGD 曾完成硬解码。当前 Linux 读到的 VENC 门控状态和原厂 HAL 兼容性也没有呈现硬件裁剪证据。JPGE 的候选 CRG 字为零、原厂样本缺少独立 JPGE 驱动，均不足以判定硬件裁剪。 [同板固件硬解记录](logs/108-reboot-usb-phy.txt#L84) · [7.3.6 验收时的固件记录](artifacts/arm64-7.3.6-aiao/final-board.txt#L84)

### 3.2 临时 RAM 内核验活

在 `hedc@ArchLinux-E5-Server` 的 `/home/hedc/hg680kd-mainline/out/arm64-7.3.6-codec-probe` 下复制隔离的 7.3.6 源码树，沿用已验收的 AIAO RAM 配置和 BusyBox initramfs。GCC 14.2.0 从空输出目录构建，编译日志零警告。诊断补丁只加入内建 `hi3798-codec-probe`，启动时注册仅 root 可写的 `/sys/kernel/debug/hi3798_codec_probe`，收到单个模块名后才执行 MMIO。测试期间未提交这些模块的 DMA 作业，也未申请它们的 IRQ。 [临时诊断补丁](artifacts/arm64-7.3.6-codec-probe/codec-probe.patch) · [构建脚本](artifacts/arm64-7.3.6-codec-probe/build.sh) · [完整构建日志](artifacts/arm64-7.3.6-codec-probe/build.log)

COM4 115200 8N1 由单进程独占。U-Boot 经 TFTP 将本机已有的原厂 BL31 副本送到 `0x08020000`，回读 CRC32 `fa236592`。随后将 17,434,696 字节的 `boot.img` 送到 `0x01fdffc0`，整镜像回读 CRC32 `104a4e28`，与主机一致。镜像 SHA256 为 `001ec43954f8c16c13072f9b6ca1f2fc217a0f101bc7aff6de0a450a8e7d65c1`。BusyBox 根位于 RAM，诊断 DTB 禁用 eMMC 节点，内核禁用 USB 大容量存储。未修改 U 盘 `/boot/boot.img`，未刷写 eMMC、操作 OTP 或执行 `saveenv`。 [boot.img](artifacts/arm64-7.3.6-codec-probe/boot.img) · [校验清单](artifacts/arm64-7.3.6-codec-probe/SHA256SUMS) · [U-Boot CRC 回读](artifacts/arm64-7.3.6-codec-probe/board.txt#L146) · [BusyBox 环境](artifacts/arm64-7.3.6-codec-probe/board.txt#L380)

按 `jpgd` → `venc` → `jpge` 顺序逐个触发。每个已开钟的引擎在读取前按原厂顺序对 SMMU、核心做复位脉冲，写后回读时钟和复位位。退出时回读完整 32-bit CRG 字，与原始值逐位比较。 [最终 dmesg](artifacts/arm64-7.3.6-codec-probe/board.txt#L603)

| 模块 | CRG 初始 → 开钟解复位 → 恢复 | MMIO 状态及默认配置读回 | 配置原值 → 写入 → 回读 → 还原 | 判定 |
| --- | --- | --- | --- | --- |
| JPGD | `+0x07c` 和 `+0x334` 均 `0x00000010 → 0x00000001 → 0x00000010` | `+0x0000=0x00000002`，`+0x0040=0x00000034`，`+0x0100=0x02000000`，`+0x0104=0x00000000` | `+0x000c` Y 步幅 bit 4，`0x00000000 → 0x00000010 → 0x00000010 → 0x00000000`，通过 | **确认存活** |
| VENC / VEDU | `+0x08c` `0x00000003 → 0x00000003 → 0x00000003`，`+0x32c` `0x00000001 → 0x00000001 → 0x00000001` | 核心 `+0x3000/+0x3004/+0x3010` 均为 `0x00000000`。SMMU `+0xf018/+0xf028` 测试前后均为 `0x00000000` | `+0x0000` 中断使能 bit 0，`0x00000000 → 0x00000001 → 0x00000001 → 0x00000000`，通过 | **确认存活** |
| JPGE | `+0x090` `0x00000000 → 请求 0x00000001，回读 0x00000000 → 0x00000000` | 开钟回读失败，未释放复位或读取 `0xf8c90000` | 条件未满足，未写入引擎配置 | **候选 CRG 不匹配** |

五个 CRG 字的最终回读全部与初值相同。JPGE 的结论限于当前候选门控位在此 Linux 环境中的表现，候选 MMIO 和芯片屏蔽状态均未验证。JPGD 和 VENC 的结果确立了寄存器响应和配置字段可写性，主线 JPEG 解码和 H.264 编码仍需各自的作业级验收。

验活前后，`/dev/video0` 和 `/dev/dri/card0` 均完成一帧 HEVC 1080p 解码与 V0 扫描输出，PXP IRQ 每次增加 1，VDP IRQ 每次增加 32，VDEC SMMU IRQ 维持 0。`/dev/snd/pcmC0D0p` 两次各播放 240,000 帧 48 kHz PCM，AIAO IRQ 每次增加 500，XRUN 为 0，设备回到 `closed`。整轮串口与 `dmesg` 未发现 SError、异步外部中止、内核 panic 或 SMMU fault/error。 [探测前回归](artifacts/arm64-7.3.6-codec-probe/board.txt#L421) · [探测后回归](artifacts/arm64-7.3.6-codec-probe/board.txt#L571) · [错误扫描](artifacts/arm64-7.3.6-codec-probe/board.txt#L649)

## 4. 开发顺序与 JPGD 最小改动

**JPGD 优先。** JPGD 有同板解码和 7.3.6 MMIO 验活证据。JPGE 先查明 MV320 的实际 CRG 或访问条件，再验证版本字和物理 DMA。VENC 已完成 MMIO 验活，可以继续分析私有 SMMU、参考帧、重建面、码流队列和码控。

1. **DTB 与 CRG。** 新增 `jpeg-decoder@f8c40000` 和专用 binding，列出 `core`、`smmu` 两段 `reg`，原厂 SPI `97`、`IRQ_TYPE_LEVEL_HIGH`，以及核心/SMMU 两个时钟和两个复位。沿用现有 CRG/reset provider，新增两个 bit 0 gate 和绑定 ID，复用按偏移/bit 解析的 reset 接口描述 `0x07c/4`、`0x334/4`。保持固件频率设置，私有 SMMU 先作为驱动资源，待控制器 binding 和通道证据明确再声明 `iommus`。 [现有板级 DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-vpu-probe.dts) · [CRG/reset provider](src/crg-hi3798mv320-reset.c)
2. **沿用本轮通过验活的时序。** 驱动 runtime PM 先开 SMMU 时钟、再开核心时钟，按原厂序列释放复位。停机按逆序收尾并检查状态。调试时 `+0x040` 默认读值 `0x00000034` 和 `+0x00c` 的步幅回读可作为基线。 [JPGD 实测序列](artifacts/arm64-7.3.6-codec-probe/board.txt#L604)
3. **V4L2 M2M 首帧。** 用 `v4l2-mem2mem` 和 VB2 DMA-contig，实现 JPEG OUTPUT、单一半平面 4:2:0 CAPTURE、单作业独占。仅支持 baseline 顺序 JPEG 的已验证采样格式。解析并校验 SOF0/DQT/DHT/SOS、步幅与缓冲区边界，按原厂 HAL 填充量化/Huffman、码流与 Y/UV 地址，最后触发启动。设 32-bit DMA mask，先验证 JPGD 允许旁路的位组合及地址范围，否则按原厂私有 SMMU 配表。完成 IRQ、错误、超时和 `STREAMOFF` 都需停止 DMA、清中断并归还缓冲区。首个验收点用小尺寸 4:2:0 JPEG 单帧对照软件解码，核对像素误差容限、一次完成 IRQ、SMMU 无故障及 CRG 回读恢复。 [JPGD 作业寄存器序列](artifacts/media-codec-audit/stock-codecs-arm32.txt#L3715) · [Linux V4L2 M2M API](https://docs.kernel.org/driver-api/media/v4l2-mem2mem.html)
