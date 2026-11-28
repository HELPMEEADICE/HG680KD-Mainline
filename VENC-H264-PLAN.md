# Hi3798MV320 VENC / VEDU H.264 静态逆向与驱动设计

2026-11-28 · 烽火 HG680KD · 原厂 ARM32 V500 R002 HAL

同板 RAM 审计已确认 VEDU 整窗 `0xf8c80000/0x10000` 可访问，核心中断使能位可写回读，CRG 两个字可恢复原值。原厂 DTB 给出高电平 SPI `101`，即 GIC INTID `133`。Linux 7.3.6 RAM 探针已通过 320×192 NV12、Constrained Baseline/CAVLC、QP 28、单 Slice IDR 的编解码验收。第 6 节记录 DMA 地址域、码流边界和总线排空实测。 [同板审计](MEDIA-CODEC-AUDIT.md#L34) · [原厂 DTB](artifacts/vpu-reverse-factory/out/factory.dts#L1135) · [历史验活读回](artifacts/arm64-7.3.6-codec-probe/board.txt#L603)

本文中 **静态已证**表示同板原厂 ARM32 指令可追到该访问或数据流。**推断**表示由调用链、数据结构或相邻 SDK 得出，涉及引擎行为的推断仍需板验。除单列的 CRG 和 SMMU 表外，`+0x…` 均相对于 VEDU 整窗。文中 `c09…`、`c0a…` 为原厂 ARM32 VA，按同板镜像基址 `0xc0008000` 可定位原始指令。基础反编译的 `pdt_init` 属于 VDEC。VENC 本身使用 `drv_venc_pdt_v500_r002_init`（`c09ced14`），寄存器以其回调和同板二进制为准。 [基础反编译](artifacts/vpu-reverse-factory/arm32-result.txt#L114) · [原厂符号表](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L31936) · [HAL 分支](artifacts/media-codec-audit/stock-codecs-arm32.txt#L17489) · [原厂 HAL 回调表](artifacts/media-codec-audit/stock-codecs-arm32.txt#L27480)

## 1. 寄存器映射与上电初始化

### 1.1 核心控制与作业配置

| VEDU 偏移 | 静态已证的写法或读法 | 字段用途及限制 |
| --- | --- | --- |
| `0x000` / `0x004` / `0x008` | 中断使能 / 清除 / 启动。`0x008 = 1` 是 HAL 最后一次 MMIO 写入 | 启动 bit 0。清除掩码和状态见第 4 节。 |
| `0x00c` / `0x010` / `0x014` | 原厂写前两字后配置作业。清总线时 `0x014 = 1` 发出请求，轮询 `0x3010 != 0`，退出写 0 | `0x00c`、`0x010` 尚无完整字段名。`0x3010` 的位定义和可涵盖的 DMA master 待验。 |
| `0x018` | `cfg_vcpi_mode` 写 `[3:2]=1`（H.264 分支），`[5]` 取低延迟开关，`[11:10]=!IntraPic`，`[16]=IntraPic` | IDR 时 `[11:10]=0, [16]=1`。P 帧取 1、0。`[12]`、`[19:18]` 跟随 profile，见第 3 节。保留其余原厂默认组合。 |
| `0x044` | `[15:0]=W-1, [31:16]=H-1` | 写可见尺寸。720p 为 `0x02cf04ff`，1080p 为 `0x0437077f`。宏块与 DMA 存储尺寸另算。 |
| `0x048` | `[15:10]` 取当帧 StartQP，其余来自 HAL 默认项 | 起始 QP。H.264 8-bit 首帧建议 QP 28，并和 PPS、Slice Header 数值相符。 |
| `0x04c` | `[20]` 为 Slice split 使能，`[17:16]`、`[15:0]` 分别装载分片模式和分片参数 | 首版关闭分片。`0x000[1]` 是单独的 Slice IRQ 使能。 |
| `0x05c` / `0x060` | `0x05c[5:0]` / `[13:8]` 分别取 RC MinQP / MaxQP。`[16]` 由 HAL 默认项写入。`0x060[5:0]` / `[13:8]` 在此路径为 0 / 51 | 固定 QP 先令 StartQP、MinQP、MaxQP 相等。`0x05c[16]`、第二组限制的硬件作用和局部 QP 修正须核实。 |
| `0x030` / `0x058` / `0x0b8` | 原厂默认写 `0x02625a00`、`0x003d0900`、`0x0001186a` | 计时相关候选字。按原厂数据保留，具体计数单位、对应状态和频率换算待查。 |
| `0x100–0x1f0` / `0x23c` | 源面、步幅、Rec/Ref、辅助面、读写指针与相关配置 | 地址和打包方式见第 2 节。 |
| `0x27c+4i` / `0x2bc+4i` | 16 对输出段字节容量和 DMA 基址，`i=0…15`，顺序已板验 | 普通单 Slice ListBuf 使用 slot 0，其他 slot 清零。 |
| `0x2fc–0x3c0` / `0x454–0x474` | 参数集地址、内容和 Slice Header 装载 | 见第 3 节。 |
| `0x3000` / `0x3004` / `0x3010` / `0x3844–0x38c0` | 核心状态、总线排空状态、逐段码流长度和结束标志 | 见第 4 节。 |

寄存器镜像还有跨 `0x01c–0x9fc` 的原厂默认写入，涉及时钟门控、参考搜索、量化/代价表、滤波等。首帧应按 `drv_venc_hal_v500_r002_init` 初始化的结构，经 `drv_venc_hal_set_default_attr_by_protocol` 和 `drv_venc_hal_set_mode_by_profile` 覆盖，再按 `drv_venc_hal_direct_cfg_reg` 逐条展开。H.264 分支还用到镜像内只读数据，如 `0xc10d1064`、`0xc10d112c` 的表。以上关键寄存器表尚不足以替代完整默认写值。所有未知字段沿用对应模式的原厂写值，并追踪地址字段的动态来源。 [原厂默认值初始化](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24435) · [协议和 profile 覆盖](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25115) · [直接配置顺序](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25322)

### 1.2 CRG 启停与复位

CRG 基址 `0xf8a22000`。以下顺序按 `drv_venc_pdt_open_hardware`、`reset_hardware`、`close_hardware` 还原。与 CRG provider 共用寄存器锁，每次变更只覆盖目标位并作回读。原厂 `+0x08c[8]=0`，父时钟和 MHz 仍未知。

| 阶段 | 操作 |
| --- | --- |
| 开钟 | 先置 `+0x32c[0]`。再对 `+0x08c` 写 `(old & ~0x103) \| 0x3`，同时打开功能时钟和 AXI 时钟。 |
| 上电复位 / 已排空的异常恢复 | 两组时钟保持开启。先置 `+0x32c[4]`，等 5 µs，清位，等 10 µs。再对 `+0x08c[4]` 执行同样的 5 / 10 µs 脉冲，复位为高有效。 |
| 空闲关钟 | 先禁止作业、处理挂起 IRQ 及排空总线。按原厂关钟顺序先清 `+0x32c[0]`，再清 `+0x08c[1:0]`。原厂的关钟回调保留 bit 4、bit 8 及其他字段。 |

静态参考的 CRG 映射取自 `drv_sys_reg_init` 和 `drv_sys_get_crg_reg_ptr`。同板历史验活时基线为 `+0x08c=3 / +0x32c=1`。如果单帧 RAM 验证临时独占 CRG，退出时回读恢复该轮保存的实际值。 [基础 CRG 映射](artifacts/vpu-reverse-factory/arm32-result.txt#L114) · [原厂开关钟及复位](artifacts/media-codec-audit/stock-codecs-arm32.txt#L27415) · [同板探针](MEDIA-CODEC-AUDIT.md#L73)

### 1.3 私有 SMMU

SMMU 子窗物理 `0xf8c8f000/0x1000`。下表偏移相对于该子窗，核心整窗偏移需加 `0xf000`。原厂 H.264 编码位于非安全路径，同时在 ISR 中采集两组 fault。

| SMMU 偏移 | 原厂直接配置或 ISR 访问 |
| --- | --- |
| `0x000` | 默认写 `0x8`。软件模板 `+0xbf4=0`、`+0xbf8=1` 分别写 bit 0、3，`[19:16]=0`。相邻 VEDU 头同位标为 `glb_bypass`、`int_en`、`ptw_pf`。 |
| `0x008` / `0x010` / `0x020` / `0x530` | 每作业原厂写 0。`0x010/0x020` 对照同系列 S/NS fault mask。其他字段待查。 |
| `0x018` / `0x028` | S / NS 状态，原厂读 `[3:0]`。 |
| `0x01c` / `0x02c` | 作业配置结束写 0。采样 IRQ 时原厂写 `0xffffffff` 清除。 |
| `0x2ec` / `0x304` / `0x30c` | NS TTBR / 读错误地址 / 写错误地址。原厂按该顺序写入三个 32-bit DMA 物理地址。安全 TTBR 路径未在该回调中启用。 |
| `0x330` / `0x340` / `0x350` / `0x360` | S 写、NS 写、S 读、NS 读 fault 地址。来源为 ISR 结构字段、日志及同系列 HAL。 |

原厂在最后阶段按顺序写 SMMU `0x000, 0x008, 0x010, 0x01c, 0x020, 0x02c, 0x530`，向核心 `0x004` 写 0，写 TTBR 及两错误地址，最后向核心 `0x008` 写 1。此前 `venc_drv_efl_create_venc` 调 `drv_venc_get_smmu_addr` 获取原厂平台页表和错误地址。复位后 Linux 方案应先创建映射并发布页表内容，在 IRQ 屏蔽期间配置 SMMU、清旧 fault，并在启动前确认地址和控制字回读。 [原厂 SMMU 作业尾序列](artifacts/media-codec-audit/stock-codecs-arm32.txt#L27241) · [TTBR 装载](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25298) · [页表地址获取](artifacts/vpu-reverse-factory/arm32-result.txt#L17043)

原厂采用混合分配。`list_buf_alloc_stream_buf`（`c09d1a18`）经 `drv_venc_alloc_and_map` 调私有 SMMU 分配，主码流和 Rec/Ref/辅助区取同一块分配内的 IOVA。参数集页和 RP/WP 页分别经 `list_buf_alloc_para_set_buf_addr`（`c09d1ae8`）与 `strm_alloc_write_read_ptr_buf`（`c09d3098`）取 MMZ 物理地址，再由 HAL 写入相应地址字段。此数据流提示两类端口可能使用不同地址域，或者 ListBuf 模式下部分硬件通道读写未启用。源面的地址域随上游分配路径变化。 [原厂 SMMU 分配](artifacts/media-codec-audit/stock-codecs-arm32.txt#L5830) · [原厂页分配调用](artifacts/media-codec-audit/stock-codecs-arm32.txt#L14328) · [原厂码流管理符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L32089)

- **页表路径有同板静态证据。** 原厂 `_hi_map`（`c0a434a0`）以 4 KiB 为步长写 LE32 PTE `(page_dma & 0xfffff000) | 1`。其域页表索引按 `(IOVA - aperture_base) >> 12`，所查域初始化将该基准置零。Linux 候选沿用仓库 VDEC 已验过的私有表形状，单独分配 4 MiB 表及隔离的错误读写页。从低于 4 GiB 的 DMA 地址建立非零 IOVA，查明各端口后在翻译端口填 IOVA，物理端口填 DMA 总线地址。首个配表作业可为指定工作区建立 `IOVA == DMA` 的恒等映射，以同一数值兼容两种端口，未映射的 PTE 保持 0。VENC 的页表尺寸/对齐下限、TLB 失效和 `TTBR` 重写行为尚未在作业上验证。第一轮使用稳定映射，待停总线并复位后再复用 IOVA。 [原厂 IOMMU 符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L34555) · [仓库 VDEC 页表实现](src/hi3798mv320-vdec.c#L206)
- **物理旁路是待验候选。** 在相同开钟复位和清旧状态后，只将 SMMU `0x000[0]` 置 1，保留 `[3]=1` 以便报告异常，等价于从原厂 `0x8` 改到 `0x9`。所有引擎 DMA 字段同时切到 32-bit DMA 总线地址，读写指针与参数集也按同一地址域准备。缺少 R002 的独立通道旁路位图，以及物理和 IOVA 混用时各 master 的选择位。SMMU 开钟、控制位回读和一次地址写入均只证明控制通路。DMA 完成、缓冲护栏和无 fault 才能作为旁路有效的证据。 [相邻 VEDU SMMU 字段](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/msp/drv/venc/venc_v1.0/drv_venc_reg_98cv200.h#L2394) · [同板 JPGD 旁路实现](src/hi3798mv320-jpgd.c#L598)

## 2. 输入与内部缓冲布局

### 2.1 NV12 / NV21

NV12 是 Y 加 UV，NV21 是 Y 加 VU。原厂手动送帧的 H.264 分支检查 `256 ≤ W ≤ 1920`、`128 ≤ H ≤ 1088`，线性源地址和步幅非零且低四位为零，步幅上限为 `0x2000`。首版接受偶数可见宽、高，按 `ceil(W/16) × ceil(H/16)` 个宏块建 SPS。硬件工作面建议取 `S=ALIGN(W,64)`、`Hp=ALIGN(H,16)`，Y 和 C 步幅均为 `S` 字节，大小分别为 `S*Hp` 和 `S*Hp/2`。64 B 来自原厂重建步幅。源读的最小安全 AXI 边界还需板验，首版 DMA 分配按页对齐并初始化末尾填充。1080 行输入在硬件工作面保留到 1088 行，SPS bottom crop 为 4 个 4:2:0 progressive crop unit。 [源校验和重建步幅](artifacts/media-codec-audit/stock-codecs-arm32.txt#L15464) · [NV12 / NV21 V4L2 布局](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-yuv-planar.html)

| 字段 | 写法 |
| --- | --- |
| 源面 Y / UV(VU) / 第三平面 | `+0x104/+0x108/+0x10c`。双平面路径第三地址保持 0。紧邻的硬件工作面可用 `C=Y+S*Hp`。 |
| 源步幅 | `+0x1f0[31:16]=Y_stride`，`[15:0]=C_stride`，单位字节。`+0x148/+0x14c[26:0]` 分别装载 `Y_stride*H` 与 `C_stride*H/2`，沿用原厂可见高度公式。 |
| 源格式 | `+0x23c[7:4]=0` 对应半平面。`[15:8]=!PackageSel`。原厂 `convert_pix_format`（`c09bc92c`）字节表基址 `0xc10cf594` 对 `0x3f/0x40` 给出 `StoreFmt=0, SampleFmt=0, PackageSel=1/0`。比对原厂 YUV 转储的 UV 分路和同系源码，NV12 对应 `[8]=0`，NV21 对应 `[8]=1`。其他位按原厂非安全、非 tunnel 组合。 |

对于 V4L2 单缓冲 NV12/NV21，用户面的 UV 起点按协商的可见 Y 行数和步幅计算。首版将用户输入搬到驱动自有的硬件工作面，1080 行 Y 在工作面填充至 1088 行。DMA 始终访问驱动自有的源和码流工作区，便于在排空失败时隔离仍有在途访问的内存。DMA 结束和地址边界均已验证后可评估直接提交用户面。 [源寄存器写入](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25693) · [打包字段](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25811) · [原厂 YUV 调试分路](artifacts/media-codec-audit/stock-codecs-arm32.txt#L23445) · [相邻 SDK 格式映射](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/msp/drv/venc/venc_v2.0/drv_venc_efl.c#L1539)

### 2.2 Rec、Ref、MV/TMV 与码流

原厂 `venc_drv_efl_create_venc` 按通道能力档而非实时帧大小预分配。720p 档 `D=1280`、Y 容量 `L=0xe1000`，1080p 档 `D=1920`、`L=0x1fe000`，每套 Y/C 合计 `E=3L/2`。原厂在 `c09c0170…c09c018c` 验证 Rec 的 C/Y 地址差至少为 `ALIGN(W,16)*ALIGN(H,16)`。创建回调验证 GOP 枚举 0/1，分别分配两或三套 Rec 和按套辅助区，共三块逐帧信息。`f` 为原厂每 GOP 的帧索引，闭 GOP 单参考的首帧 `f=0` 从两槽布局中的槽 1 写 Rec。首版保留两套，包括每帧 IDR。 [原厂分配和切分](artifacts/media-codec-audit/stock-codecs-arm32.txt#L14239) · [地址推导](artifacts/media-codec-audit/stock-codecs-arm32.txt#L14383) · [容量检查](artifacts/media-codec-audit/stock-codecs-arm32.txt#L15780)

| DMA 区 | 原厂大小、布局与用途 | 相关 VEDU 寄存器 |
| --- | --- | --- |
| Rec Y/C | 每槽 `E`，C 从该槽起点加 `L`。当前帧写槽 `(f+1)%N` | `0x194/0x198`、`0x19c/0x1a0` 写相同 Rec 地址对。 |
| Ref0、Ref1 Y/C | 同一组工作面复用，原厂地址索引 `f%N`、`(f+2)%N`。P 帧使用前帧的已完成 Rec 作为 Ref0 | `0x174/0x178` 与 `0x17c/0x180` 为 Ref0，`0x184/0x188` 与 `0x18c/0x190` 为 Ref1 相关地址对。地址槽同时被编程，参考实际启用看帧模式和列表配置。 |
| Rec/Ref 步幅 | 原厂 64-byte 对齐，Y/C 等步幅 | `0x16c/0x170=(S_Y<<16)\|S_C`，`0x160=(S_C<<16)\|S_Y`。 |
| 每槽第一组辅助面 | `T=128*ceil(D/32)*ceil(D/256)`。按 Rec/Ref 同步轮换。是否属于重建头或运动信息块待定 | `0x164/0x168` 对应 Rec/Ref0，原厂通道地址数组 `+0x85c`。 |
| 每槽第二组辅助面 | `M=128*ceil(D/32)^2`。按 Rec/Ref 同步轮换。原厂同系有 Rec 头、TMV、PME 多类对象，R002 此组的精确含义待定 | `0x124/0x128` 为 Rec 侧，`0x11c/0x120` 为 Ref0 侧，通道地址数组 `+0x868`。 |
| 按帧轮换的运动估计信息候选 | 三槽，每槽 `P=32*(ceil(D/16)+1)`。索引随 `f%3` 轮换，同系称 PME Info | `0x134/0x138` 取 `(f+1)%3, f%3`，通道数组 `+0x874`。 |
| H.264 三块额外工作区 | `A=256*ALIGN(D,16)`、`B=ceil(D/16)*ALIGN(D,256)/32`、`C=ceil(D/16)*ALIGN(D,128)/16`。分配切分精确可复算，可能包含 MV/TMV 或逐行工作区，单块算法用途待确认 | `A→0x144, B→0x140, C→0x13c`。原厂从通道 `+0x888, +0x884, +0x880` 装载。 |
| H.264 码流 DMA 工作区 | 单 Slice 用 1 段，基址和块大小按 64 B 对齐。原厂主流区预留开头 64 B，按段设置可用上限。槽的长度为字节值 | 首段容量 `0x27c`，首段 DMA 基址 `0x2bc`，后续每槽偏移加 4。探针使用独立 DMA 工作区和前后护栏。 |
| 码流 RP/WP | 原厂申请并清零 4 KiB，64 B 基对齐，RP 在 +0，WP 在 +16 | `0x130=RP_DMA`，`0x12c=WP_DMA`。在 ListBuf 中先照填。原厂 MMZ 物理地址与主码流 IOVA 的差异见第 1 节。实际单帧长度从逐段状态取。 |
| 参数集 DMA 页 | 原厂 ListBuf 申请 4 KiB、64 B 对齐。保留此页，首版仍由软件生产 SPS/PPS | `0x2fc`，`0x300–0x3c0` 为并行的长度、控制和内嵌参数表。 |
| Tunnel / DDR 配置 | 首版面向离线线性源和直接 MMIO。Tunnel 地址置 0，直接配置路径不分配 DDR 寄存器镜像 | `0x100` 来自 tunnel 地址字段，`0x010` 写寄存器配置结构内的动态字，首版保留原厂直接模式值。 |

表中的 `T/M/P/A/B/C` 对应分配公式，MV/TMV 语义未完全命名。严格按原厂区域顺序打包 `N*E`、`N*T`、`N*M`、`3*P`、`A`、`B`、`C`。原厂底层主流区申请 64 B 对齐，子区自然边界最小覆盖 32 B。在精确对齐下限和读写方向验证前保留整个工作区，并对首尾分配边界加护栏。`0x150–0x15c` 原厂写 0。`0x20c–0x220` 由帧序号及参考序号推导，为 IPPP 后续需核对的 POC/参考相关配置。 [原厂 HAL 地址写入](artifacts/media-codec-audit/stock-codecs-arm32.txt#L25705) · [原厂 CFG 入参](artifacts/media-codec-audit/stock-codecs-arm32.txt#L13316) · [同系 HAL 接口对照](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/msp/drv/venc/venc_v2.0/firmware/core/include/venc_hal_ext.h#L40)

### 2.3 单实例 CMA 预算

假设 4 KiB 页、各一份排队 OUTPUT 和 CAPTURE、独立的源与码流 DMA 工作区。用户源与 DMA 源各按 `PAGE_ALIGN(E)` 估算。码流工作区与 CAPTURE 各取 `B_s=PAGE_ALIGN(4*W*H+4096)`，包含参数集拼接和余量，属于设计预算。实际编程长度按已分配空间减去护栏对齐后写入，超出上限由 overflow 闭环处理。内部 mmz 等价区取 `PAGE_ALIGN(N*(E+T+M)+3*P+A+B+C)`，RP/WP 和参数集各一页。首版固定两套工作面，所以同分辨率下 All-Intra 和单参考 IPPP 的保守预算相同。单位 MiB 为 `2^20` B。

| 项目 | 720p | 1080p |
| --- | ---: | ---: |
| `S × Hp` | 1280 × 720 | 1920 × 1088 |
| 每套 Rec Y+C `E`（B） | 1,382,400 | 3,133,440 |
| `T / M / P`（B） | 25,600 / 204,800 / 2,592 | 61,440 / 460,800 / 3,872 |
| `A / B / C`（B） | 327,680 / 3,200 / 6,400 | 491,520 / 7,680 / 14,400 |
| 单份码流容量 `B_s`（B） | 3,690,496 | 8,298,496 |
| 两槽内部区（B） | 3,571,712 | 7,839,744 |
| **All-Intra 保守两槽** | **13.09 MiB** | **29.29 MiB** |
| **IPPP 单参考两槽** | **13.09 MiB** | **29.29 MiB** |
| All-Intra 单槽，验证后可评估 | 11.55 MiB | 25.80 MiB |
| IPPP 若使用三槽配置 | 14.63 MiB | 32.77 MiB |

私有页表加两个独立错误页约 **4.01 MiB**。每增一对队列缓冲约加 **4.84 / 10.90 MiB**（720p / 1080p）。例如含页表且双队列各两份时，两槽路径约 **21.94 / 44.20 MiB**。后续若验证可直接 DMA 读用户源面，两槽路径可节约 **1.32 / 2.99 MiB**。CMA 总量同时受 VDEC、JPGD 等队列占用和碎片影响。 [现有媒体 RAM 镜像 CMA 记录](JPGD-ACCEPTANCE.md#L37)

## 3. 码流封装、头信息与 GOP

**SPS、PPS、Slice Header 均由软件生成字段。** `flush_strm_header_h264`（`c09c38cc`）调用 `h264e_make_sps`（`c09c2ec4`）并内联打包 PPS，两个函数都先写 32-bit 值 `1`，随后写 NAL type 7/8。参数集处理存在防竞争字节的独立函数 `check_prevention_three_byte`（`c09c3c70`）。`h264e_make_slc_hdr`（`c09c33fc`）由 `venc_drv_efl_start_one_frame_venc` 调用。它将 `first_mb_in_slice`、Slice 类型、帧号、IDR 标识等编码成位串，并按 Header / 重排 / marking 交给 HAL。VEDU 将装载的头与硬件编码的 Slice 本体拼接。 [头信息调度](artifacts/media-codec-audit/stock-codecs-arm32.txt#L14038) · [Slice Header 调用](artifacts/media-codec-audit/stock-codecs-arm32.txt#L13715) · [同板头生成符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L31976)

| VEDU 偏移 | 用途 |
| --- | --- |
| `0x2fc` | 参数集 work buffer 总线地址。 |
| `0x300`、`0x304–0x3c0` | `[7:0]` 装参数集的对齐长度低字节，原厂 OR `0x1100`，`[16]` 取 IntraPic。后续装入 48 个 32-bit 参数集数据字。原厂 `get_para_set_write_buffer`（`c09cfd04`）将 SPS/PPS 的有效字节数按 64 B 对齐后交给 HAL。硬件接受的长度下限、DMA 页和寄存器表各自如何参与输出待测。 |
| `0x454–0x460` | 四个 H.264 Slice Header 位流字。已验 IDR 的第一个字右对齐，内含 slice_type、PPS ID、frame_num 和 idr_pic_id。起始码、NAL 头、first_mb_in_slice 和 QP 等剩余字段由硬件合成。 |
| `0x464/0x468`、`0x46c/0x470` | 两个重排字、两个参考 marking 字，与软件位串结构对应。 |
| `0x474` | 对软件打包的三个长度分别减一。`[5:0]=(marking_bits-1)`，`[13:8]=(reorder_bits-1)`，`[22:16]=(slice_header_bits-1)`，各按字段宽度截取。当长度为 0，相应字段由该运算得到全 1。 |
| `0x018[12]`、`[19:18]` | 原厂 profile 0（Baseline）写 1、1。非 Baseline 的 CABAC 路径将 `[12]` 清 0，profile 3 时 `[19:18]` 也为 0。结合 PPS 的 entropy flag，`[12]` 是 CAVLC 选择候选，`[19:18]` 还随变换模式变化。位域极性和独立语义须由两种码流对照验证。 |
| `0x400`、`0x478–0x49c` | 原厂直接路径的附加 Header/配置字。已定位字段写入，首版沿用 H.264 对应默认值。CABAC init、字节填充和 emulation prevention 的独立控制位待确认。 |

**输出应按 Annex-B NAL 处理。** 原厂裸 Slice Header 位串写入 `0x454` 一组寄存器时没有 32-bit 起始码。原厂 `strm_get_match_mode`（`c09d20e0`）返回 0（ListBuf），`list_buf_put_write_buf`（`c09d0ae0`）由有效长度计算对齐尾部，`list_buf_acquire_bit_stream`（`c09d0ed4`）返回 DMA 数据及去填充长度，`venc_drv_efl_task_stream` 和 `venc_drv_dbg_save_stream` 直接复制或写出这些数据。链路没有追加 Slice 起始码的步骤。因此推断正常原厂配置下 DMA 中有效 Slice 段已有起始码和 NAL Header。首帧要检查 DMA 基址处及原厂预留边界处的 `00 00 01` / `00 00 00 01`、IDR type 5，以及是否同段上载 SPS/PPS、计数是否包含头和尾。验收时按实测语义构造一份完整 Annex-B access unit，避免重复加入已随 DMA 输出的参数集。 [原厂 Slice 寄存器写入](artifacts/media-codec-audit/stock-codecs-arm32.txt#L26063) · [原厂流复制](artifacts/media-codec-audit/stock-codecs-arm32.txt#L19040) · [直接写流](artifacts/media-codec-audit/stock-codecs-arm32.txt#L23969) · [ListBuf 原厂符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L32076)

**固定 QP + 每帧 IDR 可行，实际 QP 与标准码流须验收。** 首帧从 320×192 的有明显色差图起步。设置 progressive 4:2:0、Baseline PPS entropy=0、关闭分片和局部 QP 特性，选 QP 28。令 PPS `pic_init_qp_minus26` 与 Slice `slice_qp_delta` 的和等于 2，且与 HAL StartQP 一致。沿用 stock Slice Header 分段、正常的 IDR marking、frame_num 重置和合法的 `idr_pic_id`。保留辅助面，减少在首帧同时验证的条件。 [原厂 Slice Header 准备](artifacts/media-codec-audit/stock-codecs-arm32.txt#L13584)

**IPPP 的增量**为在同一通道持有上一个已完成的重建面，按双槽轮换，IDR 后用单参考 P Slice，设置有效 L0 参考数、重排与 marking。同步更新 `frame_num`、POC、GOP 索引、参数集及参考地址。先复用固定 QP，GOP 只包含 IDR/P，不引入 B 帧和参考重排队列。P 帧完成且无错误时才提交新的参考状态。丢帧、`STREAMOFF`、复位后清空参考并重发 IDR 和 SPS/PPS。第一组 IPPP 验收用两帧有运动图案，比较软件解码和 NAL 序列类型 5 → 1，再扩展到越 GOP 周期。 [原厂参考索引配置](artifacts/media-codec-audit/stock-codecs-arm32.txt#L13210)

## 4. SPI 101、中断长度和异常恢复

### 4.1 R002 原厂 ISR 可证位图

| 事件 | 使能（`+0x000`） | 状态 | 清除（`+0x004`） | 处理 |
| --- | --- | --- | --- | --- |
| `end_of_pic` / EOF | bit 0 | `+0x3000[0]` | bit 0 | 只有 DMA 输出、计数和无故障同时成立才成功。原厂 EOF 路径清全组。 |
| Slice 完成 | bit 1 | `+0x3000[1]` | bit 1 | 首版可关闭独立使能，仍记录状态。仅在 EOF 时结束单帧。 |
| 硬件 timeout | bit 6 | `+0x3000[6]` | bit 6 | 当前帧失败并进入排空恢复。 |
| `buf_full` | 独立使能未证实 | `+0x3004[2]` | 随整组清除和复位校验，单独清位未证实 | 对同帧优先于 EOF。原厂读取后走错误或重编码。 |
| `pbit_overflow` | 独立使能未证实 | `+0x3004[3]` | 同上 | 对同帧优先于 EOF。该状态的内部计数定义待查。 |
| 总线错误 | 独立的 R002 核心 IRQ 位未知 | 总线 `+0x3010` 是排空轮询字。SMMU fault 见下 | 未证实有独立的核心位 | 原厂日志将 timeout 后排空失败视为可能的总线错误。 |
| SMMU fault | 子窗 `0x000[3]`、`0x010/0x020`，原厂写 `0x8,0,0` | 子窗 `0x018/0x028[3:0]`。bit 0 页表 miss、bit 1 页表取表错误、bit 2/3 读/写翻译错误 | 子窗 `0x01c/0x02c` 原厂写全 1 | 原厂 VENC ISR 同步读取。有无独立外部路由、错误能否单独使 SPI 101 拉高仍待确认。 |

原厂单帧使能字为 `0x41 | (slice_irq << 1)`。在 `+0x000` 的已写位中，`[5:2]`、`[13:7]` 由该路径清零。清单个来源使用 OR，对全部事件写 `0xffffffff`。驱动应在采集原始读值后按已知清除掩码写回，并核实高电平 IRQ 已释放。现有静态资料没有解释未用位、错误状态锁存/组合方式或单独 W1C 掩码。 [清除与禁止](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24235) · [状态回读](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24285) · [ISR 状态日志现场](artifacts/media-codec-audit/stock-codecs-arm32.txt#L17256)

### 4.2 实际字节数与错误闭环

原厂从 `n=0…15` 的读回对计算 `valid = (read(0x3844+8*n) & 0x1fffffff) - (read(0x3848+8*n) & 0x7f)`，后一字 bit 31 进入 `SliceIsEnd[n]`。首版只用 `n=0`。必须先检查减法无下溢、有效数据非空、两种长度均在 DMA 工作区上限内，并确认 Annex-B 起点和有效区间。H.264 `+0x3810` 是原厂乘 8 后送码控的 `PicBits` 统计，`+0x37c0` 是另一协议分支的对应计数。V4L2 的 `bytesused` 等于最终复制到 CAPTURE 的有效 NAL 字节数，含按协商加入的 SPS/PPS，不含 DMA 尾部填充。原厂 `+0x31c0/+0x31c4` 也有 27/25-bit 统计回读，首帧可采样作交叉检查。 [长度和统计读回](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24312) · [ISR 提交输出](artifacts/media-codec-audit/stock-codecs-arm32.txt#L16993)

1. IRQ 中先快照核心和 SMMU 状态及长度，随后清已服务的来源。timeout、SMMU fault、buffer full、长度越界或软件 watchdog 超期时，取消成功完成。IRQ 与 watchdog 由一次性的作业状态转换仲裁。
2. 停止调度并屏蔽核心中断，等待在途 IRQ 处理退出。在可睡眠的恢复路径写 `+0x014=1`，按原厂 `c09cba90` 条件轮询 `+0x3010 != 0`。原厂最多约 1000 次 `msleep(1)`，退出写 `+0x014=0`，成功返回 0，超时返回 -1。主线应采用有界等待，并记录最后状态。
3. 确认排空后，按第 1 节脉冲 SMMU 和核心复位。重建当前 VENC 的 SMMU 状态、默认功能字段和 IRQ 状态，令 RP/WP 初始值归零，并丢弃参考状态。当前原始/目标缓冲均以 ERROR 归还且 CAPTURE payload 为 0。下一次启动从 IDR 开始。
4. 排空失败时，按原厂 ISR 分支将其视为总线异常。保留驱动自有的 DMA 工作区、页表和 PM 引用，并停止接受作业，直到有可确认的 VENC 隔离/停机手段。用户队列未交给 DMA，可正常以 ERROR 归还。VENC 恢复只操作自己的复位域。 [原厂排空](artifacts/media-codec-audit/stock-codecs-arm32.txt#L24396) · [原厂 timeout 与复位分支](artifacts/media-codec-audit/stock-codecs-arm32.txt#L17127)

## 5. V4L2 M2M 蓝图与验证门槛

### 5.1 DT 资源

以下为将来加入 `src/hi3798mv320-fiberhome-hg680kd-arm64.dts` 的 `/soc` 内节点草案。该基础 DTS 已从根继承 GIC 父节点，并以 2/2 cells 描述地址/长度。沿用 JPGD 的基础节点与启用变体分层，使单独的基础 DTS 仍可使用。

```dts
venc: video-encoder@f8c80000 {
	compatible = "hisilicon,hi3798mv320-venc";
	reg = <0x0 0xf8c80000 0x0 0xf000>,
	      <0x0 0xf8c8f000 0x0 0x1000>;
	reg-names = "core", "smmu";
	interrupts = <GIC_SPI 101 IRQ_TYPE_LEVEL_HIGH>;
	status = "disabled";
};
```

在已有 `&crg` 的媒体变体中补充以下资源并启用。三个 clock ID 是设计提案，需新增到 binding header 和 provider，现有 JPGD clock ID 保持原值。两个 reset 使用现有的 CRG offset/bit 表达方式。bit 8 的选频动作归 CRG provider，频率字段确认后再放入时钟树。私有 SMMU 用两个 `reg` 中的后一段操作。

```dts
&venc {
	clocks = <&crg HI3798MV320_CLK_VENC_CORE>,
	         <&crg HI3798MV320_CLK_VENC_AXI>,
	         <&crg HI3798MV320_CLK_VENC_SMMU>;
	clock-names = "core", "axi", "smmu";
	resets = <&crg 0x08c 4>, <&crg 0x32c 4>;
	reset-names = "core", "smmu";
	status = "okay";
};
```

计划新增本节点的 DT binding。基础 DTS 已有 JPGD 节点，2 GiB 变体提供 `crg`，媒体 RAM 变体负责启用模块和供给 CMA。 [基础 DTS](src/hi3798mv320-fiberhome-hg680kd-arm64.dts#L87) · [CRG DT 节点](src/hi3798mv320-fiberhome-hg680kd-arm64-all-2g.dts#L42) · [JPGD 启用示例](src/hi3798mv320-jpgd.dtsi) · [现有 CRG provider](src/crg-hi3798mv320-reset.c#L369)

### 5.2 设备与单帧顺序

`hi3798mv320-venc` 拥有独立的 platform 资源、IRQ、runtime PM、私有页表/旁路状态和队列。注册一个 stateful M2M streaming 节点，`VFL_DIR_M2M`，用 VB2 DMA-contig。OUTPUT 接收 NV12/NV21，CAPTURE 用 `V4L2_PIX_FMT_H264` 每缓冲一 AU。首版 MMAP、单实例/单作业、无 B 帧，支持格式协商、能力查询、正常 drain 及 STREAMOFF 错误归还。SPS/PPS 可以和 IDR 合并，GOP 切换与 QP 控制按已验范围开放。沿板级命名要求调用 `video_register_device(…, VFL_TYPE_VIDEO, 2)`，以实际 `vdev.num == 2` 作为该镜像验收条件。V4L2 的指定号被占用时可回退到别的编号，因此本板镜像还需确认 VDEC `/dev/video0` 和 JPGD `/dev/video1` 的实际绑定。发生编号冲突时撤回 VENC 注册并报错。 [现有 VDEC 注册](src/hi3798mv320-vdec.c#L2611) · [现有 JPGD 注册](src/hi3798mv320-jpgd.c#L890) · [视频节点编号](https://docs.kernel.org/driver-api/media/v4l2-dev.html) · [Stateful 编码器接口](https://docs.kernel.org/userspace-api/media/v4l/dev-encoder.html) · [H.264 CAPTURE 格式](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-compressed.html)

1. `STREAMON` 前准备源面、内部工作区、码流 DMA、RP/WP、参数集区和可选私有页表。设置 32-bit DMA mask，验证每个 DMA 区末地址及步幅。依据第 1 节选用一致的旁路或已验证的 PA/IOVA 组合。
2. 取一对队列缓冲，将源数据搬到驱动自有的 DMA 工作面并持 runtime PM 引用。在首次上电或设备离开复位态时依第 1 节初始化。检查引擎已空闲并清旧 IRQ。生成同分辨率、同 QP 的 SPS/PPS 和单 Slice Header，建立并发布 DMA/IOVA 映射，设置该作业的 watchdog 和唯一完成状态。
3. 依据 `c09ccb2c` 展开原厂 direct 配置，IRQ 初始保持屏蔽。写控制/模式、`0x044`、QP/分片/超时字及协议默认表，然后依原厂次序写源地址/步幅、Rec/Ref、内部辅助地址、RP/WP、输出 slot 0 地址和容量、参数集以及 Slice Header。默认表和位串均按 R002 字序，按需复写同一地址字段。为首个 320×192 IDR 保留所有原厂无分片默认值。
4. 写 SMMU 控制及有效地址，清旧状态并启用 `0x000=0x41`，同步 DMA 对设备的可见性。以 `0x008=1` 作为最后一写。避免在尚未确认安全的 DMA 地址域上提交 START。
5. IRQ 按第 4 节确定结果。等 DMA 数据对 CPU 可见后检查 Annex-B、局部长度和护栏。成功则把 AU 交给 CAPTURE 并标记 keyframe，继承 OUTPUT 时间戳，归还一对缓冲，然后结束 M2M 作业。异常与 STREAMOFF 共享取消和排空流程，PM 引用延续至 DMA 安全停止。

### 5.3 验收门槛与后续测试

| 优先级 | 待确认 | 通过标准 |
| --- | --- | --- |
| P0 | `0xf000[0]` 全局旁路的实际行为、各 DMA master 地址域、NS PTE/TLB | 同一作业只给受控的 RAM DMA 地址。源、码流、辅助、参数集和 RP/WP 各自按所选地址域工作，无 SMMU fault、总线错误或周边区改写。旁路未闭环时用原厂表项格式、私有 TTBR 和 sink 页再验证。 |
| P0 | 完整 R002 默认表、保留位、`0x3010` 排空含义，`0x030/0x058/0x0b8` 计时语义 | 将单帧记录的写寄存器清单与原厂顺序逐项对照。EOF 按期出现，后续可确认总线排空。软件 watchdog 或超限后按恢复路径安全返回缓冲。 |
| P0 | `0x3844/0x3848` 长度、首个 NAL 起点、尾部填充、参数表是否同时输出 SPS/PPS | 将 DMA 差异、候选长度和 NAL 边界逐字节对齐。形成可由软件解码的 Baseline IDR，画面、尺寸、关键帧标志及 `bytesused` 一致。 |
| P1 | `buf_full` / `pbit_overflow` 的独立使能、清除、SPI 断言条件，R002 总线错误位 | 用容量受限且有护栏的 RAM 作业验证实际错误状态或 watchdog 收尾。IRQ 可清除，不污染后续作业。再测正常帧。 |
| P1 | NV21 顺序、输入垂直尾部和精确对齐、`0x018[12]` 与 CABAC、QP 实效 | NV12/NV21 用同色卡得等价输出。720p/1080p 裁剪正确。比对两档固定 QP 和实际 slice / QP 统计。CABAC 留到 Baseline 稳定后。 |
| P1 | 辅助区精确语义、最小尺寸、All-Intra 能否缩至单槽、P 参考地址和 POC | 初期保留原厂内存形状。逐项确认哪个 master 读写，完成运动图案 IDR→P 和跨 GOP 解码，再决定减少工作区。 |
| P1 | 三个媒体节点并存及共享 CMA 余量 | 验证 `/dev/video2` 编码前后，`/dev/video0` 解码与 `/dev/video1` JPEG 解码正常，三者中断及 CRG 只变化各自资源。 |

## 6. Linux 7.3.6 纯 RAM 实机验收

2026-11-28 在 HG680KD 上使用包含 Lima 0018、0019 的远端双 PP 基线源码及其 384 MiB CMA 配置。AArch64 GCC 14.2.0 从空输出目录零警告构建 `7.3.6-hg680kd-arm64-venc-probe`，产物与实机镜像逐字节相同。探针只在临时镜像注册 debugfs 节点，开机无编码作业。写入 92,160 字节 NV12 测试图后，手动向 `/sys/kernel/debug/hi3798_venc_probe/run` 写 `bypass` 或 `identity`。输出节点提供有效 DMA 段、带前后护栏的 DMA 窗口以及软件生成的 SPS/PPS。原厂 R002 HAL 在主机离线重放得到 486 条有序配置写入，DMA 地址、中断和 Slice 头由探针在提交时设置。 [探针源码](scripts/venc-probe/probe.c) · [寄存器提取](scripts/venc-probe/extract-registers.py) · [干净构建记录](logs/534-venc-probe-7.3.6.txt#L4)

| TFTP 装载项 | 字节数 | SHA256 | U-Boot RAM 回读 CRC32 |
| --- | ---: | --- | --- |
| `boot.img`，`0x01fdffc0` | 19,826,760 | `018f9c8fcc7822a01705dd41ee85852b0d124b343b064da09018d5de1042bd2c` | `543f395b` |
| 原厂 BL31 RAM 副本，`0x08020000` | 262,144 | `5f32da6063d8c49d0c2cc9fc7a1667f0e2b75931ce4d73af842f3efc51a97b7b` | `fa236592` |

COM4 以 115200 8N1 独占。测试镜像和 BL31 经 TFTP 装入 RAM，二者 CRC32 均匹配后执行 `bootm`。根为 BusyBox RAM rootfs，未暴露 eMMC、USB 大容量存储或 MTD 块设备。测试命令未写入 U 盘 `/boot/boot.img`、eMMC，也未执行 OTP 操作或 `saveenv`。 [TFTP 与旁路串口记录](logs/534-venc-probe-7.3.6.txt#L44)

### 6.1 DMA 地址域与复位

- 首轮将输出地址和容量两个槽写反。旁路触发后内核在 `__ipi_send_mask` 崩溃。U-Boot 自动重启已被串口拦截。私有恒等表复测捕获 NS 写 fault `0x8`，fault 地址 `0x0003d000` 恰为编程容量，错误写入被 sink 页接收。由此确认 `0x27c` 是容量，`0x2bc` 是 DMA 基址。 [首轮旁路异常](logs/534-venc-probe-7.3.6.txt#L12) · [私有表定位](logs/534-venc-probe-7.3.6.txt#L25)
- 槽位修正后，旁路 `0xf000=0x9`、`TTBR=0`，私有恒等表对照为 `0xf000=0x8`。两种路径的输入、重建 Y/UV、辅助工作区及码流输出通过本单帧验证。重建和辅助区观测到写入，输入、护栏和 sink 页保持原值。参数集页及 RP/WP 页本作业未观测到写入，读取行为仍待专项验证。每帧 INTID 133 增加 1，`0x3000=1`、`0x3004=1`，Slice IRQ、timeout 和两项 overflow 位均为 0。VENC 私有 S/NS fault 和 VDEC SMMU INTID 181 在修正后的各次采样均为 0。两路径的 IDR DMA 段逐字节相同。 [恒等表通过](logs/534-venc-probe-7.3.6.txt#L33) · [旁路通过](logs/534-venc-probe-7.3.6.txt#L44)
- `0x3010` 在 START 前、START 后、EOF 及发排空请求前均读 0。写 `0x014=1` 后首次轮询读到 1，撤销请求后仍为 1，按原厂顺序复位 SMMU 和核心后回到 0。中断、长度与 fault 状态在复位后清零，CRG `+0x32c/+0x08c` 完整恢复 `0x1/0x3`。同一 RAM 会话连续发起独立 IDR 作业通过。排空超时隔离路径已实现，实测成功路径未触发该分支。 [排空与连续作业](logs/534-venc-probe-7.3.6.txt#L44)

### 6.2 码流边界与双解码

- `0x3844=0x00000180`，`0x3848=0x8000000f`，得到 `384 − 15 = 369` 字节，`0x3810=0x171` 同为 369。DMA 基址首字节为 Annex-B `00 00 00 01 65`，整段仅一个 IDR NAL。第 369 字节为 `e0`，后接 15 个零填充字节，384 字节边界后的 64 字节仍为 `cd`。前置 64 字节护栏仍为 `a5`。FFmpeg 严格模式可解码全部 369 字节，去掉最后一个有效字节后报告末宏块损坏且无输出帧。 [首尾 64 字节 dump](logs/534-venc-probe-7.3.6.txt#L60) · [截尾对照](logs/534-venc-probe-7.3.6.txt#L83)
- 软件在 DMA IDR 前拼接 20 字节 SPS/PPS，完整 AU 为 389 字节。FFmpeg 严格解码零错误，解析为 320×192 Constrained Baseline/CAVLC、单 Slice IDR，240 个宏块 QP 均为 28。以原始 NV12 为参照，Y/U/V PSNR 分别为 **51.4301 / 50.3043 / 48.1037 dB**。 [完整码流](logs/534-venc-probe-frame.h264) · [结果 JSON](logs/534-venc-probe-result.json) · [宏块 QP 日志](logs/534-venc-probe-7.3.6.txt#L98)
- 以同一 AU 的 SPS/PPS 和 Slice 字段构造 stateless V4L2 请求，把原始 IDR NAL 提交给板端 `/dev/video0`。VDEC PXP INTID 169 增加 1。串口取回的 92,160 字节 NV21 与 FFmpeg 输出逐字节一致，SHA256 均为 `e8731f975d95edbbcf47f2d6e37f63a23881ea0357cc7f6b5e1dfee5d0d25974`。 [VDEC 环回](logs/534-venc-probe-7.3.6.txt#L91)

### 6.3 回归

Lima 的单 PP、双 PP、交替任务分别通过 16、16、32 个 4096 像素比对任务，INTID 126 分别增加 32、32、64。Mesa 25.2.7 通过 `/dev/dri/renderD128` 用 Mali450 渲染 60 帧 640×480 GLES，INTID 126 增加 180。JPGD `/dev/video1` 通过 1920×1080 JPEG，INTID 129 增加 1。VDEC/VDP 通过 2160p60 HEVC Main10 播放，解码与上屏各 1,080 帧，零丢帧、零漏 vblank。AIAO 播放 864,000 帧 48 kHz PCM，零 XRUN。VDEC、VDP、AIAO 的 IRQ 分别增加 1,080、1,090、1,800。回归期间的 INTID 181 采样均为 0，内核日志无故障。 [回归串口日志](logs/534-venc-probe-7.3.6.txt#L114) · [Mesa GLES 日志](logs/534-venc-probe-7.3.6.txt#L126)

### 6.4 重建与复测

使用 `scripts/remote.ps1` 及本地 DPAPI 凭据将 `probe.c`、`crg-venc-probe.inc`、`r002-registers.h`、`probe.dtsi`、`scripts/pack_arm64.py` 和补丁 0019 上传到 `/home/hedc/hg680kd-mainline/out/venc-probe/input/`。远程脚本复制现有双 PP 源树，只在副本接入探针，输出至 `out/venc-probe/release/`。取回 `boot.img` 和 `SHA256SUMS` 至本地结果目录。镜像与完整原始记录位于构建工作区的 `artifacts/arm64-7.3.6-venc-probe/`。

```powershell
$hgKey = (Get-ChildItem -LiteralPath $env:TEMP -Filter 'hg680kd-ssh-*.dpapi' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
& scripts/remote.ps1 -PasswordFile $hgKey script scripts/venc-probe/build.sh /home/hedc/hg680kd-mainline

$dir = 'artifacts/arm64-7.3.6-venc-probe'
python -u scripts/venc-probe/accept.py $dir --log "$dir/retest-bypass.txt"
python -u scripts/venc-probe/accept.py $dir --log "$dir/retest-regression.txt" --running-ram --regression-only
python -u scripts/venc-probe/accept.py $dir --log "$dir/retest-mesa.txt" --running-ram --mesa-only
```

复测脚本需要现有验收件中的 BL31 RAM 副本、静态 BusyBox、Lima/JPGD 测试工具和 4K 播放样本，主机需安装 FFmpeg、FFprobe 和 pyserial。从已启动的 USB Armbian shell 切换时加 `--leave-usb`，串口会拦截重启到 U-Boot 后走 TFTP。 [远程构建脚本](scripts/venc-probe/build.sh) · [串口验收脚本](scripts/venc-probe/accept.py)
