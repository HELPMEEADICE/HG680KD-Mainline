# HG680KD 多媒体硬件资源摸底

2026-11-28 · 烽火 HG680KD / Hi3798MV320 · Linux `7.3.6-hg680kd-arm64-usb`

JPGD 已在同一块板的原厂 U-Boot 中完成过 1280×720 Logo 硬解码。Linux 7.3.6 上已读取 JPGD、VENC 的 CRG 及芯片型号状态字，三块引擎的解复位后 MMIO 响应仍待验证。现有 7.3.6 内核只暴露 CRG/PERI 的只读窗口。JPGE 有同板原厂 DTB 的 IRQ 证据，MMIO 和 CRG 定义来自相邻型号。 [原厂串口记录](logs/108-reboot-usb-phy.txt#L89) · [本轮 COM4 读回](artifacts/media-codec-audit/mainline-snapshot.txt)

## 1. 硬件资源总表

| 模块 | 物理 MMIO 范围、长度 | GICv3 SPI / INTID | CRG `0xf8a22000` | DMA / SMMU | 板端判定 |
| --- | --- | --- | --- | --- | --- |
| JPGD | 核心 `0xf8c40000–0xf8c406ef`（`0x6f0`），SMMU `0xf8c4f000–0xf8c4f363`（`0x364`）。均为原厂映射长度。 | `97` / `129`，高电平 | 核心 `+0x07c`，SMMU `+0x334`，各 bit 0 开时钟、bit 4 置位复位。此版驱动未设置频率选择 | 私有 SMMU。原厂走页表，物理/CMA 方案取决于旁路验证。 | **确认可用**（同板原厂 U-Boot 硬解码）。7.3.6 引擎 MMIO **需进一步验证**。 |
| JPGE | `0xf8c90000–0xf8c9ffff`（`0x10000`），Hi3798M V200 地址图。该 SDK 驱动映射前 `0x10c0`。均为 MV320 候选。 | `102` / `134`，高电平 | 候选 `+0x090` bit 0 开时钟、bit 4 置位复位。相邻型号定义无选频字段 | 相邻型号 HAL 将 MMZ 物理地址写入引擎，SMMU 输入先由 TDE 搬到 MMZ。MV320 待确认。 | **需进一步验证**。 |
| VENC / VEDU | `0xf8c80000–0xf8c8ffff`（`0x10000`）整窗。核心状态组在 `+0x3000`，SMMU 位于尾部 `0xf8c8f000–0xf8c8ffff`（`0x1000`）。 | `101` / `133`，高电平 | 核心 `+0x08c` bit 0/1 开核心/AXI 时钟、bit 4 置位复位、bit 8 原厂清零。SMMU `+0x32c` bit 0 开时钟、bit 4 置位复位 | 私有 SMMU。原厂为编码通道准备页表和错误地址，物理旁路待验证。 | **需进一步验证**。CRG 已开且匹配原厂 HAL 的芯片版本。 |

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

## 3. Linux 7.3.6 板端只读探测

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

本轮在初值读取后停止。开钟、解复位、引擎寄存器读回和恢复闭环均未执行。没有板端 CRG 或引擎寄存器写入、没有重新编译或重启，也没有访问 eMMC、OTP、U 盘 `/boot/boot.img` 或执行 `saveenv`。

同板历史 U-Boot 日志在 `LOGO Flag` 后记录 `jpgd_dfx_print_timestamp ... hard in 1280 720 out 1280 720 ... dec 4`，证明该机 JPGD 曾完成硬解码。当前 Linux 读到的 VENC 门控状态和原厂 HAL 兼容性也没有呈现硬件裁剪证据。JPGE 的候选 CRG 字为零、原厂样本缺少独立 JPGE 驱动，均不足以判定硬件裁剪。 [同板固件硬解记录](logs/108-reboot-usb-phy.txt#L84) · [7.3.6 验收时的固件记录](artifacts/arm64-7.3.6-aiao/final-board.txt#L84)

## 4. 开发顺序与 JPGD 最小改动

**JPGD → JPGE → VENC。** JPGD 有同板解码证据和可复用的原厂寄存器序列。JPGE 首先要得到 MV320 的独立映射与版本读回。若物理 DMA 通路得到确认，单帧编码主要覆盖量化、输入面与码流缓冲区。VENC 还需确认私有 SMMU、参考帧、重建面、码流队列和码控，最后推进。

1. **DTB 与 CRG。** 新增 `jpeg-decoder@f8c40000` 和专用 binding，列出 `core`、`smmu` 两段 `reg`，原厂 SPI `97`、`IRQ_TYPE_LEVEL_HIGH`，以及核心/SMMU 两个时钟和两个复位。沿用现有 CRG/reset provider，新增两个 bit 0 gate 和绑定 ID，复用按偏移/bit 解析的 reset 接口描述 `0x07c/4`、`0x334/4`。保持固件频率设置，私有 SMMU 先作为驱动资源，待控制器 binding 和通道证据明确再声明 `iommus`。 [现有板级 DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-vpu-probe.dts) · [CRG/reset provider](src/crg-hi3798mv320-reset.c)
2. **先完成只读验活。** 在下一版 RAM 诊断驱动中申请寄存器资源，先保存两字 CRG 原值并确认设备空闲。按原厂顺序使能 SMMU 和核心时钟、回读并等待，再依次释放两个复位。只读取 `+0x000/+0x040/+0x100/+0x104` 等明确字，区分合法的零状态与整组无响应。若仍需读写鉴别，在 DMA 未启动、IRQ 屏蔽且映射已证实的条件下，对核心 `+0x00c` 的步幅字段做一次窄掩码写回并立即还原。退出时先恢复复位位，再恢复门控位并回读，原本开启的位依快照恢复。JPGE 先补足同型号映射证据，VENC 初次只采状态，使用各自资源序列。
3. **V4L2 M2M 首帧。** 用 `v4l2-mem2mem` 和 VB2 DMA-contig，实现 JPEG OUTPUT、单一半平面 4:2:0 CAPTURE、单作业独占。仅支持 baseline 顺序 JPEG 的已验证采样格式。解析并校验 SOF0/DQT/DHT/SOS、步幅与缓冲区边界，按原厂 HAL 填充量化/Huffman、码流与 Y/UV 地址，最后触发启动。设 32-bit DMA mask，先验证 JPGD 允许旁路的位组合及地址范围，否则按原厂私有 SMMU 配表。完成 IRQ、错误、超时和 `STREAMOFF` 都需停止 DMA、清中断并归还缓冲区。首个验收点用小尺寸 4:2:0 JPEG 单帧对照软件解码，核对像素误差容限、一次完成 IRQ、SMMU 无故障及 CRG 回读恢复。 [JPGD 作业寄存器序列](artifacts/media-codec-audit/stock-codecs-arm32.txt#L3715) · [Linux V4L2 M2M API](https://docs.kernel.org/driver-api/media/v4l2-mem2mem.html)
