# Hi3798MV320 VDEC 原生 Tiled 输出可行性调研

2026-11-27 · 烽火 HG680KD · Linux 7.3.5

**优先验证 VDEC 原生 CAPTURE，再实现 VDP V0 直扫。** 本机原厂代码已经把 VDH 的非压缩 Tiled 帧映射到 V0 的视频取数模块，并按位深配置主 Y/VU 与低 2-bit Y/VU。VPSS0 同样能接收这两组面，有线性 8-bit 和紧凑 10-bit 写回证据。其 16 bit/sample 写回分支值得验证，当前还不能将其输出承诺为标准 P010。本轮依据本地代码、二进制和既有板测日志，硬件像素、实时吞吐及 HDMI 深色输出仍需板测。

## 1. 方案对比

| 方案 | 8-bit / Main10 | 4K60 可行性 | 逆向与开发工作量 | FFmpeg / 播放器兼容性 |
|---|---|---|---|---|
| **A · VDP V0 Tiled 直扫** | 原厂寄存器配置支持 8-bit VU 和 Main10 8+2 分离面。V0 能力表给出 4096×2304、10 bit | 优先验证。消费端只需读一遍原生面。已有 2160p60 HDMI 时序记录 | 高。V0 关键字段已定位，需新建 MV320 KMS、打通 dma-buf、vblank 和缓冲区生命周期 | 需 V4L2 fourcc 到 DRM modifier 映射及支持该格式的 KMS 直扫路径 |
| **B · VPSS0 硬件转线性** | 8-bit NV21 有原厂通路。Main10 有 2-bit 读通路与 10-bit 紧凑写回。P010 待证 | 有条件。核对实际写回尺寸、单节点耗时及并行带宽。原厂用户端口有 2048×1080 限制 | 中高。需恢复一条渐进式 UHD 节点、读写通道、SMMU 与完成中断 | 线性 NV21 / 经证实的 P010 可复用现有 FFmpeg 映射。紧凑 10-bit 要按布局协商 |
| **现有 CPU + NEON** | 线性 NV21 / P010 已有逐帧哈希 | 实测 23.62 / 15.23 FPS。解排布单项 17.46 / 44.95 ms，均超过 16.67 ms | 已实现，可作像素参考与回退 | 当前 FFmpeg Request 分支已验证下载和校验。播放仍取决于渲染端 |

性能对照见 [24 轮 CSV](logs/483-vdec-cache-measurements.csv) 和 [逐帧回归](VDEC-PERFORMANCE.md#默认-direct-与-cacheable-tiled-输出)。VDP 证据见下文。原厂 U-Boot 曾输出 2160p60 YCbCr 4:2:0 的 AVI VIC 97，GFX0 显存为 1080p。此记录证明固件时序先例，Main10 扫描输入、HDMI 输出位宽和每个 vblank 更新各自需要验证。[HDMI 实测记录](HDMI-REVERSE.md#L19)

## 2. A · VDP 直扫

现有 7.3.5 镜像使用 U-Boot 初始化的 GFX0 和 `simple-framebuffer` / `simpledrm`。配置启用 Lima，现有补丁没有 MV320 VDP KMS。需要用原生驱动接管固件输出，最小实现可沿用启动时的 HDMI link，接管 HD0、V0、aperture 和 vblank，随后再补独立 modeset、HDMI bridge 和热插拔。[DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-all-2g-hdmi.dts#L10) · 7.3.5 镜像配置（`kernel.config:L2529`） · [固件接管设计](HDMI-REVERSE.md#L90)

原厂 `vdec_convert_frm` 把非压缩 Tiled VU 标为格式 **99**，按帧位深附带低位地址和 stride。窗口适配的跳转表把 99 映射到 VID 格式 **7**，UV 顺序控制值为 **0**，压缩类型为 **0**。FDR 路径将格式 7 写成 `data_fmt=1, data_type=2`，位宽值 **0/1** 对应 8-bit / 8+2-bit。位宽为 1 时同一分支额外写低位 Y/VU 两组地址和各自 pitch。格式 102 走含压缩头的分支，本研究使用 99。厂商能力表报告 V0 上限 4096×2304 / 10 bit，V1 为 4096×2304 / 8 bit。优先选 V0。帧格式转换（`hg680kd-stock-kallsyms.txt:L25430`） · 窗口适配（`hg680kd-stock-kallsyms.txt:L17790`） · FDR（`hg680kd-stock-kallsyms.txt:L17879`） · 视频层能力表（`hg680kd-stock-kallsyms.txt:L20727`）

| V0 偏移，基址 `0xf8cc0000` | 配置 |
|---|---|
| `+0x8240` | `[2:0]=2`、`[4:3]=1`、`[9:8]=0/1`、`[21:20]=0`，分别为取数类型、4:2:0 格式、数据位宽、解压类型。渐进式 2D 路径另外配置 `[19:16]` display mode |
| `+0x8210[4]`、`+0x8220[1:0]` | VU 顺序控制为 0。后者为主色度/亮度 2D 取数的 SMMU bypass 位 |
| `+0x8250/54`、`+0x8258/5c` | 主 Y、VU 各自的地址高/低字 |
| `+0x8280/84`、`+0x8288/8c` | Main10 低位 Y、VU 各自的地址高/低字。原厂普通路径写低字，本机原型仍应核验地址宽度和高字初值 |
| `+0x8290`、`+0x8294` | 主面、低位面 stride，两个字均为低半字 Y、高半字 VU。原厂 FDR 直接写描述符里的 stride，证据支持沿用 VDH 字节 pitch |
| `+0x8244/48/4c`、`+0x2000/04` | 源尺寸、裁剪、取数尺寸。VID enable 为 `+0x2000[31]`，`+0x2004` 写 1 更新。FDR 与 VID 每层间距分别为 `0x200`、`0x1000` |
| `+0xe000[0]`、`+0xe2ec` | 显示块 SMMU 全局 bypass、非安全 TTBR。原厂配置路径取私有页表地址并写 VDP 自己的寄存器组 |

当前已知工作约束为主面 pitch `S=ALIGN(w,256)`、分配高 `H=ALIGN(h,64)`、低位 pitch `S/4`，3840×2160 对应 3840 / 960 B、分配高 2176。应将 256 B 作为此 modifier 的生产者约束，VDP 所能接受的最小 pitch、地址和裁剪对齐仍待实测。VDP 可走自己的 SMMU，也有 bypass 候选。原型可用 DMA-contig/CMA 面，在 VDP attachment 的 DMA 地址上验证 bypass 及所有取数端口，尤其是低位通道。VDH 的 `0x10000000` 起始私有 IOVA 只在 VDH 页表有效。原厂曾为各引擎返回同一套页表参数，主线仍需按设备映射和管理地址。[VDH 映射](src/hi3798mv320-vdec.c#L208) · [布局](src/hi3798mv320-vdec.c#L2140) · 显示 SMMU（`hg680kd-stock-kallsyms.txt:L19274`）

## 3. B · VPSS0、TDE 与 GFX2D

**VPSS0 的物理窗口为 `0xf8cb0000`，原厂映射 `0xf540` 字节。** CRG 基址 `0xf8a22000`，`+0xf0[0]` 为时钟开关，`[4]` 控制复位，原厂在 VPSS `+0x340/+0x364` 轮询。VPSS0 的 SMMU 全局 bypass 为 `+0xf000[0]`，非安全 TTBR 为 `+0xf2ec`，UHD 节点用 `drv_smmu_get_page_table_addr` 取页表和错误地址。DT 只命名了 IRQ。原型优先验证独立 SMMU 或每设备 DMA-contig bypass，按 VPSS attachment 的 DMA 地址编程。VPSS 初始化（`hg680kd-stock-kallsyms.txt:L28273`） · CRG（`hg680kd-stock-kallsyms.txt:L28336`） · SMMU（`hg680kd-stock-kallsyms.txt:L28244`）

原厂 VDEC 通过 `vdec_chan_create_vpss` 绑定 VPSS 实例和事件回调。帧描述沿 `vdec_convert_frm → vpss_inst_set_hal_frame_info → vpss_ctrl_start_2d_task / vpss_hal_set_task_info → vpss_hal_set_2d_uhdframe_node` 进入任务节点。UHD 路径将格式 99 标为 Tiled 4:2:0。位宽为 1 时构造第二组地址和 pitch，`cbb_mac_set_rchn_cfg` 使用 `vpss_rchn_set_{y,c}2b_{addr_l,stride}`。读通道以 `+0x5080` 为配置基址，低位地址高/低字为 `+0x50d0/d4`、`+0x50d8/dc`，低位 stride 为 `+0x50e0`，以上偏移还需加读通道的寄存器偏移。VDEC 通路（`hg680kd-stock-kallsyms.txt:L25689`） · UHD 节点（`hg680kd-stock-kallsyms.txt:L28313`） · 读通道（`hg680kd-stock-kallsyms.txt:L28541`）

写回通过 `vpss_hal_set_port_cfg → cbb_cfg_frm_write_chn → cbb_mac_set_wchn_cfg`，写通道基址 `+0x5800`，有位宽、存储类型、Y/C 地址、字节 pitch 和尺寸字段。原厂线性 4:2:0 VU（格式 64）和 UV（63）都在映射表中。两者的 `vpss_osal_cal_buf_size` 位宽枚举 1 使用 `ALIGN(ceil(10*w/8),64)`，枚举 3 使用 `ALIGN(2*w,64)`。因此 **VPSS 可合并 8+2 并写回 10-bit 采样**，常规路径采用紧凑存储。枚举 3 和 `vpss_hal_set_cts10_node` 提供了 16-bit 容器的继续调查线索。要确认 P010，需证明该枚举在 UHD 写回通道生效、每个 LE16 字的高 10 位承载采样、低 6 位为零，并用 U/V 不同的样本验证 UV 顺序。按当前枚举表，8-bit 线性 pitch 使用芯片分支的 16 或 32 B 对齐，写通道校验还要求 pitch 为 16 B 整数倍。原厂 `vpss_inst_sync_usr_cfg` 会调用 `vpss_check_port_cfg_output` 限制显式用户端口尺寸至 2048×1080。UHD 输入节点的存在只能证明 UHD 输入能力，首轮应测试实际写满 3840×2160 的输出。写通道（`hg680kd-stock-kallsyms.txt:L28231`） · 线性缓冲区计算（`hg680kd-stock-kallsyms.txt:L27778`） · CTS10（`hg680kd-stock-kallsyms.txt:L28301`） · 用户端口限制（`hg680kd-stock-kallsyms.txt:L28005`）

最小实现从单入单出、渐进式、等尺寸 UHD 节点裁剪，关闭 DEI、NR、HDR、缩放、旋转和隧道。原厂每个节点分配 `0xf540` B，并复制 `0xf534` B 的寄存器模板。单节点的可变字段预计约 **20–40 个**，集中在 MAC、读写通道、旁路、SMMU、任务启动和 IRQ，模板默认值仍需复核。链表首地址和启动为 `+0x2fc`、`+0x300[0]`，清中断和 mask 寄存器为 `+0x308`、`+0x324`。原厂 ISR 区分完成与故障状态。应验证 DMA 描述符发布顺序及超时后的停机和复位。工作量低于重做 VDH 的 H.264/HEVC 任务消息、参考帧和熵解码配置。`vpss_func_cfg_trans` 的 `+0x5040/44/48` 仅出现搬运参数，当前应使用视频读写通道验证解排布。节点分配与模板（`hg680kd-stock-kallsyms.txt:L28286`） · 启动（`hg680kd-stock-kallsyms.txt:L28199`） · ISR（`hg680kd-stock-kallsyms.txt:L28078`） · [VDH 工作量参照](VPU-REVERSE.md)

**TDE/GFX2D 从本轮实现候选排除。** TDE 映射 `0xf8c10000 + 0x40a4` 字节，私有 SMMU 为 `0xf8c1f000`，CRG `+0x94` 和 `+0x330` 的 bit 0/4 分别做开关时钟/复位，另轮询 `+0x174[5]`。GFX2D HWC 报同一基址与 IRQ 123，并映射 `0xf8c104fc`、`0xf8c14000` 子窗。现有证据支持共享硬件资源，未定位 GFX2D 独立 CRG/SMMU 配置。TDE 的普通栅格和 MB blit 以及 GFX2D HWC 图形源描述均未找到 VDH 的行交换与 2-bit 旁平面对应项。硬件仍可能有未暴露模式，当前没有足够依据接入本布局。TDE 初始化（`hg680kd-stock-kallsyms.txt:L21233`） · TDE 时钟与 SMMU（`hg680kd-stock-kallsyms.txt:L21305`） · GFX2D HWC（`hg680kd-stock-kallsyms.txt:L30754`）

## 4. V4L2、DRM 与用户态

- **V4L2 CAPTURE。** 保留现有线性 NV21/P010，增加明确约定布局的两个 MV320 fourcc。现有 CAPTURE 已是 `VIDEO_CAPTURE_MPLANE`，但 `num_planes=1`，VB2 为 MMAP + vmalloc，`EXPBUF` 得到的是线性成品。首版原生格式建议仍用一个 V4L2 内存 plane、一份 VB2 DMA-contig 缓冲和一个 dma-buf。用 `bytesperline=S` 与布局文档确定分区。令 `Y=S*H`，8-bit 的 Y/VU 偏移为 `0,Y`，Main10 再追加 `3Y/2,7Y/4` 两个低位偏移。两种格式的 `sizeimage` 分别为 `3Y/2`、`15Y/8`，分配时另做必要的页对齐。Y 主面为 64 B×16、VU 为 64 B×8，主面按横向 tile 内行号 XOR 寻址。低面按 64 采样×2 行，4 个低位采样低位优先打包成 1 B。MPLANE 也可以定义 2/4 个内存 plane，当前 VDH 任务依赖同一 IOVA 内的相对偏移，分对象需要重做连续 IOVA 拼接和校验。PMV 与保护区继续留在私有 DMA 分配，按现有硬件约束保持 IOVA 邻接。导出范围限于视频面和必要的对齐填充。[布局和位序](src/hi3798mv320-vdec.c#L762) · [队列与 PMV](src/hi3798mv320-vdec.c#L1757) · [HEVC 低位消息](src/hi3798mv320-vdec.c#L1287)
- **DRM。** 8-bit 可表达为 `DRM_FORMAT_NV21` + MV320 私有 modifier。Main10 为 10-bit 4:2:0 VU 的显式 fourcc/modifier 组合，定义四个存储平面，逐个提供 offsets/pitches。若将来用既有 10-bit fourcc 作逻辑格式，其 modifier 规范必须明确辅助面数、分离存储及 UV/VU 的语义。线性 P010、NV15、现有 NV12 tiled fourcc、通用 tile 名称和其他厂商的 modifier 各有不同存储契约。Linux 7.3.5 的 V4L2 MPLANE 协商走像素 fourcc，DRM 通过 Plane `IN_FORMATS` 和 `ADDFB2` modifiers 表达组合。单个 dma-buf 的 GEM handle 可在两个或四个 FB plane 重用。Main10 若使用四个存储 plane，需修改 KMS 对该格式的 FB 布局校验并提供对应 format info。原型在配对驱动间使用私有值，公开 ABI 前核对该版本 UAPI 和 libdrm 命名空间。当前本地构建产物未带完整 7.3.5 UAPI 源树，本轮未核对可注册编号。[本地构建入口](scripts/build_usb_vdec.sh#L5) · [现有 DRM 格式映射补丁](scripts/ffmpeg-v4l2request-nv21.patch)
- **DMA 生命周期。** 原生 CAPTURE 由 VDH 直接写入，工作区和码流沿用现有路径。完成 IRQ 后发布缓冲和生产者依赖。开始下一次写入前确认 DPB、KMS 扫描或 VPSS 读取已完成。KMS 用 page-flip 完成事件或 out-fence 判定旧 FB 可释放，需同时遵守 V4L2 时间戳引用、请求持有和 streamoff 生命周期。可去掉当前 CPU detile 路径的 surface sync。保留非一致性平台上实际发生的 CPU 访问同步、dma-buf attachment 映射、内存屏障和设备间完成依赖。现行每帧清整面需单独验证，优先只优化解码器确实覆盖的范围，保留硬件会读的 padding/guard 和 PMV 初始化。[同步](src/hi3798mv320-vdec.c#L265) · [完成路径](src/hi3798mv320-vdec.c#L891) · [现有 CMA 配置](scripts/build_usb_vdec.sh#L31)
- **用户态。** 先扩展 `vdec-request-test.c`，吞吐模式 DQBUF 后回收，无需 mmap 像素。显示模式用 `EXPBUF → drmPrimeFDToHandle → drmModeAddFB2WithModifiers → atomic commit`，协商 `IN_FORMATS` 并按 vblank/fence 回收。项目 FFmpeg 使用 Kwiboo `v4l2-request-n8.1` 的固定提交，改 `hwcontext_v4l2request.c` 的 CAPTURE 格式表、选择逻辑以及 `AVDRMFrameDescriptor` 的 plane/object/modifier 构建，沿用 Request 和 DPB 生命周期。自定义 Tiled 格式的软件下载需专用转换。mpv 的 `drm-copy` 会回到 CPU 路径，`drm` 能否直接显示取决于该构建选择的 FFmpeg hwdevice / DRM PRIME interop，以及 EGL/GPU 或 KMS overlay 是否支持该组合。直接 V0 扫描优先验证自定义 KMS 程序，随后接 mpv 的 DRM PRIME overlay 或对应 VO。本轮以仓库 FFmpeg 补丁和构建记录为准，mpv 的选项与行为需在目标构建复核。[测试程序](src/vdec-request-test.c#L65) · [FFmpeg 构建](scripts/build_vdec_ffmpeg.sh#L5) · [NV21 映射](scripts/ffmpeg-v4l2request-nv21.patch)

## 5. 4K60 吞吐和 DDR 预算

同组 240 帧、三轮均值。原纯硬件解码约 9.515 ms（Main）/ 9.535 ms（Main10），倒数均约 105 FPS。加现有 RBSP 的阶段估算约 99 / 101 FPS。维持其他开销，从现有 FFmpeg 无显示墙钟中扣除 detile 和 surface sync，剩余约 **16.65 / 14.87 ms，60 / 67 FPS**。若还能省掉原每帧整面清零的大部分成本，按整项扣除的乐观估算约 **83 / 86 FPS**。以上是同片源反推，后续需测 P95/P99、实际重排深度、积压及长期热稳态。直接扣减不计入新增的 dma-buf、fence 或显示竞争，8-bit 保留清整面时几乎没有余量。[计时原始数据](logs/482-vdec-cached-perf.txt#L677) · [均值数据](logs/483-vdec-cache-measurements.csv)

| 3840×2160 @ 60，十进制 GB/s | 8-bit | Main10 |
|---|---:|---:|
| 原生像素字节/帧，含高度 padding | 12,533,760 | 15,667,200 |
| 仅 VDEC native 输出写回 | 0.752 | 0.940 |
| 方案 A：VDEC 写 + VDP 扫描读 | 1.504 | 1.880 |
| 方案 B：VDEC 写 + VPSS 读写，紧凑线性 NV21 / P010 | 2.251 | 3.373 |
| 方案 B 再加一次线性扫描读 | 2.997 | 4.866 |

原厂启动记录为 LPDDR4-2666、32-bit，总线理论峰值 **10.66 GB/s**。上表按整面流量计，使用紧凑可见尺寸的线性输出，未计 DPB、PMV、码流、cache 回写、控制器效率和其他设备。相对理论峰值，A 尚余 9.16 / 8.78 GB/s，B 加扫描后尚余 7.67 / 5.80 GB/s。每增加一遍等效 DPB 全面读取，又消耗 0.752 / 0.940 GB/s。实测有效余量取决于当前 DDR 频点、参考局部性、读写交替和 QoS，4K60 验证应同时跑解码与消费者。在 384 MiB CMA 预算下，17 个 2160p CAPTURE 参考面含 PMV 约占 **237 / 288 MiB**，另需工作区和显示持有缓冲。三张 P010 写回面还需约 71 MiB。需要按实际 DPB 深度、stride、页面分配和背压验证容量。[DDR 配置](logs/225-hdmi-reboot-to-uboot.txt#L24) · [CMA 和参考面](VPU-REVERSE.md#L395) · 板端 CMA 日志（`serial-usb-vdec-boot2.txt:L320`）

## 6. 硬件证据索引

原厂二进制、反汇编、符号表和设备树的本地目录为 `artifacts/vpu-reverse-factory/`。其中 `factory.dtb` 位于 `inputs/`，`factory.dts` 位于 `out/`。7.3.5 镜像配置和串口日志位于 `artifacts/arm64-7.3.5-armbian-usb-vdec/`。

以下 VA 为原厂 ARM32 内核地址。已有 `arm32-result.txt` 的导出范围主要是 VDEC，未包含 V0、VPSS0 的关键配置例程。补查使用同一份 原厂 kernel（`hg680kd-stock-kernel.bin`）、IDA 数据库（`stock-arm32.i64`） 和 符号表（`hg680kd-stock-kallsyms.txt`），按 VA 核对 ARM 指令及跳转表。表中行号指向既有导出、符号表或 DTS，VA 可用于复查新增证据。

| 证据 | 文件行号 / 关键 VA |
|---|---|
| 原厂 DTB `/soc/virtdev`，GIC SPI、flags 4 | factory.dts L1135–1138。直接解析 factory.dtb 复核索引 12/13/14/15，VDP **90/122**、TDE **91/123**、GFX2D **91/123**、VPSS0 **93/125**（SPI/INTID）。该节点只有 `compatible` 和两项 IRQ 属性，无独立 `reg/clocks/resets/iommus` |
| VDH 输出与通用私有页表 | arm32-result L5737（`arm32-result.txt`） `dec_hal_calc_dec_mem_param c077b9bc`。L14534–14600（`arm32-result.txt`） Y/C 主面及低位读取符号，主面寻址补见 vdh-layout-extra（`vdh-layout-extra.txt`）。L4（`arm32-result.txt`） `drv_smmu_get_page_table_addr c0552f38`，L17043（`arm32-result.txt`） `get_iommu_ptable_addr c0a43054` |
| VDP MMIO / CRG / 最大位宽 | kallsyms L20151（`hg680kd-stock-kallsyms.txt`） `disp_init c0642a5c`，`c0642b20–28` 传入 `0xf8cc0000`。L17736（`hg680kd-stock-kallsyms.txt`） `disp_hal_init c05dd988` 映射 `0x1da74`。L20727（`hg680kd-stock-kallsyms.txt`） V0 能力表 `c066a560`。[CRG +0xd8/+0x34c](HDMI-REVERSE.md#L66) |
| VDP 格式与地址 | kallsyms L25430（`hg680kd-stock-kallsyms.txt`） 解码帧描述 `c0823f50`。L17790–17799（`hg680kd-stock-kallsyms.txt`） 窗口适配，`c05e0acc` 跳转表项 `99-9` 指向 `c05e0d74`。`xdp_drv_set_vid_layer_fdr` L17879（`hg680kd-stock-kallsyms.txt`） `c05e7684–c05e76b0` 写低位地址和 pitch。FDR setter L18298–18341（`hg680kd-stock-kallsyms.txt`），`c05f973c` 插入位宽，`c05fa1b4/c05fa28c` 写主/低位 Y pitch |
| VDP SMMU 与更新 | kallsyms L17748（`hg680kd-stock-kallsyms.txt`） `vdp_hal_set_smmu_enable c05de590`。L19276/L19295（`hg680kd-stock-kallsyms.txt`） `c0610c5c/c0611364` 写 bypass / TTBR。L19328（`hg680kd-stock-kallsyms.txt`）、L19371（`hg680kd-stock-kallsyms.txt`） `c0611e58/c0613618` 使能与更新 |
| VPSS0 MMIO / CRG / SMMU | kallsyms L28274–28275（`hg680kd-stock-kallsyms.txt`） `vpss_hal_init c08d6a7c` 映射窗口。L28336–28337（`hg680kd-stock-kallsyms.txt`） `c08e21a4/c08e22a0` 配置 CRG。L28244（`hg680kd-stock-kallsyms.txt`） `c08d4064/c08d4094` 调用 bypass/TTBR setter。`vpss_osal_mem_map` arm32-result L15379（`arm32-result.txt`） 调用 `drv_smmu_map` |
| VPSS0 UHD 读写与任务提交 | kallsyms L28313（`hg680kd-stock-kallsyms.txt`） `c08ddde0–c08dde04` 装载低位面，`c08dde68/c08ddeb4` 配置读通道和输出 port。L28230–28234（`hg680kd-stock-kallsyms.txt`） `c08d2850–c08d2880` 写低位面，`c08d3464` 写输出位宽，`c08d34cc–c08d34fc` 写输出地址和 pitch。L27778（`hg680kd-stock-kallsyms.txt`） `c08ad9d0/c08ad9f4` 为紧凑 / 16-bit stride 分支。L28707（`hg680kd-stock-kallsyms.txt`） PNEXT/START，L28078（`hg680kd-stock-kallsyms.txt`） IRQ |
| TDE/GFX2D | kallsyms L21233（`hg680kd-stock-kallsyms.txt`） `drv_tde_module_init_k c06a09e0`。L21305–21306（`hg680kd-stock-kallsyms.txt`） `c06a735c/c06a7390` 映射设备和 SMMU，`c06a6f24–c06a6f68` 设置页表和 fault 地址。L21388–21389（`hg680kd-stock-kallsyms.txt`） MB 路径。L30755–30775（`hg680kd-stock-kallsyms.txt`） GFX2D MMIO `c097d458/c097d4d4`、能力表 `c097edec` |

## 7. 推荐落地顺序

1. **短期，先测 VDEC 上限。** 增加单 dma-buf 原生 CAPTURE，软件解排布只用于离线验样。用相同 Main/Main10 片源测无显示的持续 FPS、阶段耗时和 P95/P99，确认像素、DPB 回归、PXP 和 SMMU 计数。再测有真实低位变化的 Main10、参考帧密集片源以及非整 tile 边缘。先量化保留清零时的预算，再验证能否缩小清零范围。
2. **短期，平行消除 V0 与 VPSS 两个硬件疑点。** V0 沿固件 2160p60 时序，先用预制 Tiled 彩条校验 Y/VU、裁剪和行交换，再用只改变低两位的 Main10 图案确认低位读通路，最后验证 60 Hz page flip 和 DMA 复用。VPSS0 先做独立一帧渐进式、等尺寸 NV21 写回，核对末行、pitch 和缓冲区保护带，再对比 bitw 1/3 的原始字节并测连续 4K60 写回。两条路径测试均以像素一致、稳定帧计数和无总线故障为通过条件。
3. **中长期，A 作实用播放主线，B 提供线性互操作。** V0 先完成 KMS 原生 plane、PRIME 和 FFmpeg 映射，再补播放器直扫、HDMI modeset、BT.709/BT.2020 色彩、HDR 与输出位深协商。VPSS0 保留异步 V4L2 mem2mem 转换入口的设计，8-bit 先交付线性 NV21，10-bit 以实测得到的布局协商。现有 CPU/NEON 用于对照和回退。端到端验收以持续 60 帧解码、显示或写回、无掉帧及混合负载下的热稳态为准。
