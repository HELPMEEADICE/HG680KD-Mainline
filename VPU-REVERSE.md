# HG680KD Hi3798MV320 视频解码逆向

2026-11-22，烽火 HG680KD 板型 4，2 GiB LPDDR4。Linux 7.3.5 经 TFTP 纯 RAM 启动，已验证 32×32 CAVLC IDR 和 Main Profile CABAC IDR/P 的 VDH 硬解。两种码流按 tiled VU 布局回读，转换为 I420 后逐字节匹配。原厂线性请求及实测见第 8 节。[解码串口](logs/431-vpu-h264-final-serial.txt) · [外设回归](logs/413-vpu-final-regression.txt) · [重复探测](logs/414-vpu-repeat-mmio-and-storage-hdmi.txt)

## 1. 原厂材料

原厂内核为 ARM32 Linux `4.9.118_hi3798mv320`。用 `debugfs` 缺省只读模式在原厂 Android 9 的 `system`、`vendor` eMMC 副本上扫描库、服务、编解码 XML、固件和模块目录。八个 `vendor/lib/modules/*.ko` 为无线、SDIO 或蓝牙。视频解码在内核中以 `vdec_*`、`vfmw_*`、`dec_hal_*`、`scd_hal_*` 和 `hal_h264_*` 编入。已扫描的目录中无独立 `hi_vfmw.ko`、`hi_vdh.ko`、`hi_scd.ko`、`hi_smmu.ko` 或 `vfmw_hal` 文件。Android OMX AVC 组件通过 `/dev/hi_omxvdec` 进入内核。原厂播放器 API 所在的 `libhi_msp.so` 含 `/dev/hi_vdec`。另有依赖 `libavcodec.so` 的 `libHV.VIDEO.FFMPEG_VDEC.decode.so`。[文件清单](logs/404-vpu-factory-inventory.txt) · [ELF 依赖与设备节点](logs/412-vpu-factory-elf.txt)

| 材料 | SHA256 |
| --- | --- |
| [原厂 boot.img](artifacts/vpu-reverse-factory/hg680kd-stock-boot-template.img) | `97f4e2b5d88ee6d6fae8ced4809420d7f875c4ff8b0acfe817c6409b6f02a6b8` |
| [原厂解包内核](artifacts/vpu-reverse-factory/hg680kd-stock-kernel.bin) | `a47592ad5a842035f0bd6e41fdce71cb89c2238aff57e2b333d60213af176724` |
| [板端 dtbo 内 FDT](artifacts/vpu-reverse-factory/inputs/factory.dtb) | `e289000306072f9fd2e140dc5a837fb646b9c803dae15c2b4d0b99c32cf63517` |
| 原厂 `system.img` 只读副本 | `06cf51fffea3214b38945c5c322334ccb4456f87b7c381db86442811e0ef1206` |
| 原厂 `vendor.img` 只读副本 | `726274205f81c1d79ab85da9c7c5f4cf10e8462cbc77b01a5a2a3edf440740b0` |
| [libOMX.hivendor.video.decoder.so](artifacts/vpu-reverse-factory/extracted/vendor/lib/libOMX.hivendor.video.decoder.so) | `af67f4999bd4bcbf70e361465a5739d6a4400e9126a41b86e51dd6d84716b8e9` |
| [libhi_msp.so](artifacts/vpu-reverse-factory/extracted/vendor/lib/libhi_msp.so) | `b3beff7cb2cefd46e79fff19631240004689ddabe1517140b58d45bdcd095672` |

用恢复的 kallsyms 构造 ARM little-endian ELF，以 `_text=0xc0008000` 对齐镜像，调用 IDA Pro 9.3 的 `idat.exe` 和 IDAPython 批量分析 217 个候选函数，对伪码有间接调用、栈变量或 literal pool 歧义的部分交叉核对 A32 指令。下文原厂函数地址均为 kallsyms VA。原厂 SDK 字符串为 `HiHuanglong_V100R009C01SPC023_v2024111912`，可由内核 `0xc13bb71c` 与 U-Boot 串口独立核对。[IDA 数据库](artifacts/vpu-reverse-factory/stock-arm32.i64) · [批处理输出](artifacts/vpu-reverse-factory/arm32-result.txt) · [符号表](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt)

## 2. MMIO、GIC 与硬件标识

| 窗口 | 物理地址 | 原厂证据 |
| --- | --- | --- |
| VDH 主窗口 | `0xf8c30000–0xf8c30fff` | `dec_hal_open_vdh` `0xc077a5fc` 映射 0x1000 |
| VDH 普通 PXP 寄存器和任务槽 | 主窗口 `+0x020` 和 `+0x100` | `dec_hal_open_vdh` 初始化两组偏移。另一上下文使用 `+0x300`、`+0x400` |
| SCD | `0xf8c39c00–0xf8c39fff` | `scd_hal_open` `0xc076a298` 从基址映射 0x1000，已见访问止于 `+0x4c`。诊断 DT 取到下一个已知窗口前的 0x400，避免资源重叠 |
| VDH 全局控制 | `0xf8c3a000–0xf8c3afff` | `pdt_open_hard_ware` `0xc081d1bc` 映射 0x1000 |
| VDH 私有 SMMU | `0xf8c3d000–0xf8c3dfff` | `pdt_map_mmu` `0xc081ceec` 与 `dec_hal_open_vdh` 均映射 0x1000 |
| BPD / MFDE | 独立边界待确认 | 有 `vdh_bd` IRQ 名及 VDH BSP 指示器。当前二进制未建立 BPD、MFDE 名称与独立 MMIO 区的对应 |
| CRG | `0xf8a22000`，VDEC 使用 `+0x78` | `drv_sys_reg_init` `0xc055eb54`、`pdt_enable_vdh` `0xc081d004` |
| SoC 标识及平台字 | `0xf8000ee0`、`0xf8a20840` | `pdt_init` `0xc081d320` 分别保存至 `vdh_glb_reg+0x18/+0x1c` |

原厂 FDT 在 `/soc/virtdev` 中按索引配对 `interrupt-names` 与 `interrupts`。触发条件均为 GIC SPI、高电平（flags `4`）。没有独立的视频解码节点提供 `reg`、`clocks`、`resets`、`power-domains` 或 `iommus`。该驱动使用的物理窗口由上表中的原厂代码确定。[原厂 FDT 反编译](artifacts/vpu-reverse-factory/out/factory.dts)

| 原厂名称 | SPI | GIC INTID | 对照 |
| --- | ---: | ---: | --- |
| `vdec_vdh` | 95 | 127 | 原厂 DT 的另一路 VDH 项 |
| `vdec_scd` | 104 | 136 | `stm_dev_irq_init` 使用 `0x88` |
| `vdec_vdh_safe` | 105 | 137 | 原厂 DT |
| `vdec_scd_safe` | 106 | 138 | 原厂 DT |
| `vdh_pxp_safe` / `vdh_pxp` | 136 / 137 | 168 / 169 | `dec_hal_init_irq_vdh` 另一上下文 `0xa8`，普通上下文 `0xa9` |
| `vdh_bd_safe` / `vdh_bd` | 138 / 139 | 170 / 171 | 原厂 DT，BD 完成路径待确认 |
| `mmu_vdh_safe` / `mmu_vdh_s` | 148 / 148 | 180 / 180 | 原厂 DT 共用 INTID，另一上下文 `0xb4` |
| `mmu_vdh` | 149 | 181 | 普通上下文 `0xb5` |

现场 U-Boot 和主线都读到 SoC 标识 `0x37980320`、平台字 `0xc0100000`。可确认的解码硬件接口为该 SDK 的 VFMW VDH/PXP 与 SCD。原厂 `vfmw_get_version` `0xc08076e0` 固定返回 `1`，`dec_hal_read_dec_reg` `0xc077c1b0` 固定返回 `0`，SCD 诊断打印只包含任务寄存器。VDH/SCD 独立 revision 和 `VDH_V500`、`V5R2`、HiVXE 版本归属仍待确认。[U-Boot 读数](logs/406-vpu-uboot-registers.txt) · [主线重复读数](logs/414-vpu-repeat-mmio-and-storage-hdmi.txt)

## 3. CRG、PMC 与复位

1. `pdt_enable_vdh` 对 `0xf8a22078` RMW，清 bit 0、bits 9:8、bits 17:12，等价于 `value &= 0xfffc0cfe`。`pdt_disable_vdh` 只置 bit 0。U-Boot 交接读数为 `0x00000000`。诊断镜像把这组原厂启停动作封装为 VDEC CCF gate。
2. `dec_hal_open_vdh_clock` `0xc077a084` 再清 CRG 的 bits 9:8、17:12，并在 VDH 全局 `+0x04` 的七个 2-bit 字段逐字段写 `2`。因此低 14 位为 `0x2aaa`，保留高位。`pdt_set_clk` 仅打印 `not complete yet`。各字段命名、PLL 父源、分频和 VDEC 实际 MHz 尚无本机证据。
3. `dec_hal_reset_vdh` `0xc077ad1c` 在全局 `+0x04[1:0]` 非零时保存 PXP `+0x08/+0x18`，置全局 `+0x0c[1]`，每 30 µs 检查全局 `+0x14[1]`，最多 1000 次。清请求后恢复两个 PXP 屏蔽寄存器。诊断额外检查应答去置位。本机每次都观察到请求过程中 ACK `0x2`，释放后请求和 ACK 为 `0x0`。
4. 原厂 `scd_hal_enable`、`scd_hal_disable`、`scd_hal_reset` 都是空操作。SCD 共享已通电的解码通路。诊断在 `SCD+0x04` 写 `1` 屏蔽中断，再在 `+0x08` 写 `1` 清中断。没有提交 DMA 或触发 SCD。

这些初始化和关停函数未访问 PMC `0xf8a23000`。在本机固件交接条件下，仅用上述 CRG 和 VDH 全局复位即能完成探测。独立 VPU MTCMOS、隔离开关与完全去电后的上电动作仍未知，因此 DTS 只描述已证实的 CRG 资源，PXP 软复位留在设备驱动内。现有 CPU/核心 DVDD 证据见 [温控与 DVFS 逆向](DVFS-THERMAL-REVERSE.md)。

## 4. VDH 私有 SMMU

原厂普通路径确实配置私有页表。`pdt_map_mmu` 经 OSAL 取得 `drv_smmu_get_page_table_addr` `0xc0552f38` 返回的三个物理地址，底层为 `get_iommu_ptable_addr` `0xc0a43054`。本机在没有初始化页表的探测阶段，`SMMU+0x00=0x1`、`+0x28=0`，PXP 软复位前后相同。SCD 的 `scd_hal_is_bypass` / `stm_dev_is_scd_bypass` 属于码流预处理选择。另一处 VDP 的 `vdp_smmu_set_glb_bypass` `0xc0610c5c` 写显示 SMMU bit 0。这使 VDH `+0x00[0]` 成为旁路候选，仍需用实际 DMA 证实每个 master 的地址路径。

| 偏移 | 原厂普通上下文配置和读回 |
| --- | --- |
| `+0x000` | `pdt_map_mmu` 写 `0x8` |
| `+0x020` | `pdt_map_mmu` 写 `0`。每任务由 `dec_hal_write_reg` 按任务 `+0x89` 切成 `0/0xf`，与非安全故障中断控制相邻 |
| `+0x028/+0x02c` | `dec_dev_mmu_int_process` 读非安全故障状态，原厂向 `+0x02c` 写 `0xf` 清除 |
| `+0x2ec` | 32-bit 平坦页表的物理基址，原厂写入字节地址 |
| `+0x304/+0x30c` | 读、写故障缓冲物理基址，原厂各分配 512 B，并向上对齐至 256 B |
| `+0x340/+0x360` | 故障处理程序回读的地址寄存器 |

`hi_iommu_probe` `0xc0a4374c` 分配 `0x400000` 字节的物理连续页表。`hi_domain_alloc` 将它映射为非缓存，清零后执行 `dmb ishst`。`_hi_map` `0xc0a434a0` 写入 4 KiB 页的单级 PTE，索引为 `(IOVA - aperture_start) >> 12`，原厂平坦域起点为 0，每项 4 B，值为 `(page_phys & 0xfffff000) | 1`。`hi_map` 与 `hi_map_range` 最终都用 `dmb ishst` 发布。只读解析和实际写回均应使用分配得到的 32-bit DMA 地址，不能沿用原厂 ARM32 的虚拟地址换算常数。

第一帧建议从原厂普通 SMMU 路径开始。先在停机状态分配并清零页表、异常缓冲和工作内存，为本次作业涉及的所有页面填表，在 DMA 可见性屏障后按上述偏移写表基址和故障地址。页表走 DMA API，IOVA 分配仅涵盖实际工作范围。映射撤销与重用前的私有 TLB 失效协议仍需确认，首次可使用新表和新 IOVA，保留缓冲直到复位与停机均完成。SCD 的地址翻译是否和 VDH 共表仍需单独实测。[Linux DMA API](https://docs.kernel.org/core-api/dma-api.html)

## 5. H.264 单帧寄存器与消息

原厂路径为 `stm_dev` / `scd_hal` 预处理码流，`h264_dec_slice` 解析帧及 slice，`dps_h264_make_dps_slice` 生成解码参数，`hal_h264_write_msg` 填充公用、图片、slice 或单 SED 消息，最终 `dec_hal_config_reg_vdh`、`dec_hal_start_dec` 启动作业。原厂是按消息驱动的硬件接口，VDH 需要由软件准备 H.264 语法字段和工作缓冲。

| SCD 偏移 | 写入或读回 |
| --- | --- |
| `+0x00` | 在 OSAL 写屏障之后写 `0` 再写 `1` 启动，见 `scd_hal_start` `0xc076a664` |
| `+0x04/+0x08/+0x0c` | IRQ mask（1 为屏蔽、0 为使能）、清除写 1、完成状态 bit 0 |
| `+0x14` | 编码类型低 4 位。其余由原厂工作模式组合到 bit 4、7、9、13，见 `scd_hal_write_reg` `0xc076b084` |
| `+0x18/+0x1c` | `pre_lsb/pre_msb` 上次分段遗留字节 |
| `+0x20/+0x24/+0x28` | 下行、上行消息池地址除以 4，以及上行容量字段。原厂两区为 `0x5000`、`0xc000` 字节 |
| `+0x2c/+0x30/+0x34` | `buffer_first/last/init`。前两项为码流边界地址除以 4，init 保留 `scd_hal_set_reg` 输出的原始值 |
| `+0x38` | 置 0 |
| `+0x18/+0x1c/+0x28/+0x3c–0x4c` | 完成后由 `scd_hal_read_reg` 读取，`+0x40` 拆成高 10 位和低 22 位，`+0x44` 返回量乘 4 |

| 普通 VDH/PXP 偏移 | 首帧还原所需字段 |
| --- | --- |
| `VDH+0x00/+0x04` | 各写 `0xaaaaaaaa` |
| `VDH+0x08/+0x14` | `0x000b0003` 与 task byte `+0x88 << 6` |
| `VDH-global+0x08` | 写 `1` |
| `PXP0+0x04/+0x08/+0x18` | 原厂提交前分别写 `1/1/0` |
| `PXP0+0x00/+0x0c` | BSP 指示器及 PXP 当前状态。复位后原厂确认 `+0x0c=0` |
| `PXP0+0x10/+0x14` | 中断状态与清除。状态含 low 16-bit 标记、bits 17/18/19 和 bits 26:24。原厂以状态 bit 17 生成 clear bit 0，以 bit 19 生成 clear bit 2 |
| `PXP0+0x20/+0x24/+0x28` | `dec_hal_read_int_info` 附加完成状态 |
| `VDH+0x100+n*0x20+0x04` | 任务头。普通上下文 bit 0 为 0，任务 byte `+0x8a[0]` 同时写入 bits 8、9，task halfword `+0x0e` 写入 bits 27:12 |
| `VDH+0x100+n*0x20+0x08` | 作业消息 DMA 地址除以 16 |
| `VDH+0x100+n*0x20+0x00/+0x10` | 内存写屏障后依次写 `1/1`。见 `dec_hal_start_dec` `0xc077b554` |

`dec_hal_write_core_msg` `0xc077b3e0` 在 DMA 消息内用 `+0x00/+0x14` 写工作地址除以 16，`+0x70/+0x80` 写每组地址除以 16，`+0x90/+0xc0/+0xd0` 填原厂计数和参数。`dec_hal_open_vdh` 的分配分支使用工作区偏移 `0x32000/0x32c00/0x33c00/0x34000`，普通小图所需独立区域、大小以及缓冲生命周期要跟随实际消息路径验证。

H.264 公共消息的 word 2 含宏块总数减 1 和模式高字节 `0x41/0x4d`，word 31 含宏块宽高各减 1。图片消息 word 0 含同样的宽高及 SPS/PPS 派生位，word 1 至 4 携带图片参数，word 10 起携带引用帧地址除以 16，word 27 起携带引用信息对，word 64 至 119 由原厂表拷贝。通用帧地址位于公共消息 word 9，另一个平面按基址加偏移后除以 16 写 word 20。`hal_h264_write_msg` `0xc07a9dcc` 在 core message 的 `+0x500/+0x540/+0x580` 生成参考帧地址组，`+0x614`、`+0x61c` 为当前帧的派生缓冲地址，`+0x618` 来自任务 stride 参数。原厂分支可能使用压缩或 tiled 输出，校验前要明确 `pdt_get_dec_size` 和 `pdt_yuv_2d` 的表面布局。

`hal_h264_write_slice_msg` `0xc07a91c8` 给出了 VDH 的相对码流寻址。设 `base` 为公共消息 H.264 descriptor `+0x48` 的码流基址，`ptr` 为 slice descriptor `+0x14`，`bit` 为其 `+0x10` 的 byte。公共消息 word 6 为 `base >> 4`，slice 消息 `+0x00` 为 `((ptr & ~15) - (base & ~15)) >> 4`，`+0x04` 为 `(ptr * 8 + bit) & 0x7f`，`+0x08` 为 descriptor `+0x08` 传入的非零长度。无第二段时 slice 消息 `+0x0c/+0x10/+0x14` 清零。slice 参数另写在 `+0x18–0x2c`、`+0xb0`、`+0xf4/+0xf8/+0xfc`。正常 slice 列表和 `hal_h264_set_sed_only` 由帧模式选择，单 slice 输入仍需按普通解码路径确认模式。

下一轮限定为一帧 Constrained Baseline、逐行、单 slice、无参考历史。优先闭合以下证据，再启用 DMA。

1. 核对 SCD 下行记录的长度和低位地址格式，再跟踪上行 segment 到 DPS 的转换，确认 IDR slice 的地址、位偏移和长度单位。
2. 从 `hal_h264_write_pub_msg`、`hal_h264_write_pic_msg`、`hal_h264_write_slice_msg` 还原上述 DMA 消息中的 SPS/PPS、slice、量化矩阵、参考帧和工作缓冲字段。把 VDH 寄存器中的任务头与各消息 DMA 槽对应起来。
3. 明确 SMMU 事务和错误检测，注册普通 PXP INTID 169、SCD 136、SMMU 181 的处理程序。复位后只配置第 0 槽、提交一次，分别记录 IRQ 状态、完成条件与 fault 状态。超时须先停机并确认复位应答再释放 DMA 布局。
4. 校验输出 stride、tile 或压缩模式，转为 I420 后与软件参考逐像素比对。保留初始填充值以判定目标平面确实发生硬件写回。

已生成 [32×32 IDR Annex B 码流](artifacts/vpu-reverse-factory/h264-idr-32x32.264) 和 [软件 I420 参考](artifacts/vpu-reverse-factory/h264-idr-32x32.i420)。码流只有 SPS、PPS、IDR，SHA256 `c81b3ab07fbecf8d5f3570f080904a02ca82229c168a4d65ff2f74707e284ee8`。软件帧 1536 B，SHA256 `ffcb5e22013d3f42d9260f7c1e54f2bb082876dda77c7231c6088f331139f186`。这组数据用于下一轮硬解比对。

## 6. Linux 7.3.5 镜像与复现

`arm64-7.3.5-vpu-probe` 继承 DVFS RAM 基线配置和 DTS。VPU 节点仅加入该目标。驱动映射资源，核对普通世界 IRQ，执行 CRG 启用、全局 PXP reset 和 SCD IRQ 屏蔽清除，并记录 SoC 标识与空闲 MMIO。GIC IRQ 号分配已验证，诊断驱动尚未注册解码 IRQ handler。[诊断 DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-vpu-probe.dts) · [探测驱动](src/hi3798mv320-vpu-probe.c) · [GCC 构建日志](logs/409-arm64-7.3.5-vpu-final-build.txt)

| 镜像 | SHA256 |
| --- | --- |
| [VPU RAM 探测 boot.img](artifacts/arm64-7.3.5-vpu-probe/boot.img) | `555e2875db0eb4833129a8087fedf6c3b4f82541e8c19754836daf998c1390b9` |
| [现役 RAM 回退 boot.img](artifacts/arm64-7.3.5-all-2g-hdmi-bt-dvfs/boot.img) | `3c8cb9aab2c60da79206d043067ab23bbe7b9bb1643996d3c5e6b668ca017811` |

在构建服务器同步 `src/` 与 `patches/` 对应文件到 `inputs/`，并同步 `scripts/build_arm64.sh`。构建后取回 `out/release-arm64-7.3.5-vpu-probe.tar.gz`。

```sh
root=/home/hedc/hg680kd-mainline
bash scripts/build.sh full "$root" "$root/inputs" arm64-7.3.5-vpu-probe
```

```powershell
$out = 'artifacts/arm64-7.3.5-vpu-probe'
python scripts/pack_arm64.py --image "$out/Image" `
  --dtb "$out/hi3798mv320-fiberhome-hg680kd-arm64-vpu-probe.dtb" `
  --out $out --name boot.img
```

COM4 使用 115200 8N1。运行项目的 TFTP 服务，U-Boot 通过 RAM 会话变量指定服务器与端口，沿用基线的只读 `mmc read 0 0x8020000 0x10000 0x1000` 固件准备动作。将镜像传到 `0x01fdffc0`，比对完整 CRC32 `74d7d72c` 后执行 `bootm 0x1fdffc0`。本次未修改持久化 U-Boot 环境、eMMC 或 OTP，也未覆盖 USB `/boot/boot.img`。[最终 TFTP 和串口](logs/411-mainline-arm64-7.3.5-vpu-final-boot.txt)

## 7. 实机结果

| 项目 | 最终镜像实测 |
| --- | --- |
| VPU | 启动探测成功后连续三次解绑、重绑。每次 SoC 标识 `0x37980320`、平台字 `0xc0100000`、VDH 时钟字段 `0x2aaa` 一致，PXP ACK 置位并去置位。PXP state、PXP IRQ、SCD IRQ、SMMU IRQ 均为 0，SCD mask 和 SMMU ctrl 均为 1。无 SError 或 Data Abort。[复测串口](logs/414-vpu-repeat-mmio-and-storage-hdmi.txt) |
| CPU 和内存 | `0-3` 在线，`MemTotal=2022420 KiB`。[启动串口](logs/411-mainline-arm64-7.3.5-vpu-final-boot.txt) |
| CPU DVFS / 温控 | 200、400、600、800、1000 MHz 均与读回一致。TSENSOR 读到 56 °C，`soc-thermal` 及 68 °C 被动 trip 在线，CPU/GPU 冷却设备已注册。[外设回归](logs/413-vpu-final-regression.txt) |
| Lima / GPU DVFS | 200 和 300 MHz 两档读回正确，每档两次 GP+PP 离屏测试均 4096/4096 像素匹配。[外设回归](logs/413-vpu-final-regression.txt) |
| RTL8761BTV 蓝牙 | 固件版本 `0x1ed6b420`，10 秒被动扫描收到 2 条广播。[外设回归](logs/413-vpu-final-regression.txt) |
| GMAC | `100 Mb/s full`，对构建服务器 ICMP 3/3。[外设回归](logs/413-vpu-final-regression.txt) |
| eMMC | 整盘及 system/vendor 均 `ro=1`，8-bit HS400ES 192 MHz。原厂 boot 容器 12,304,384 B 哈希匹配，SDHCI 计数为零。[整盘与哈希](logs/413-vpu-final-regression.txt) · [错误计数](logs/414-vpu-repeat-mmio-and-storage-hdmi.txt) |
| USB 和 HDMI | 只读挂载 USB 启动分区，`boot.img` SHA256 与实验前相同，USB 链路 480 Mb/s。1080p fbdev 回读 1080 行，采集卡 24/24 个 RGB 采样点匹配。[只读校验和 fbdev](logs/415-vpu-usb-and-fbdev.txt) · [采集日志](logs/416-vpu-hdmi-capture.txt) · [采集图](artifacts/arm64-7.3.5-vpu-probe/hdmi-color-bars.png) |

测试结束后重启，按原有 U-Boot 启动策略恢复 USB Armbian，并进入登录流程。[恢复串口](logs/417-vpu-return-to-usb.txt)

## 8. H.264 输出布局与 CABAC IDR/P 实测

本节记录提交 `943f430` 的历史原型实验。当前构建目标和用户态 API 见第 9 节。

### VDH 线性请求

原厂 `vfmw_com` 的 `linear <0/1>` 由 `vct_log_set_2d_en`（`0xc0787788`）写入默认通道配置。线性配置经 `dec_hal_calc_mem_param` 写任务字节 `+0x98`。该字节的 bit 0 由 `hal_h264_write_pub_msg`（`0xc07a8f18/0xc07a8f2c`）送入 `pub[3][29]`。同一任务字节决定 `dec_hal_calc_dec_mem_param` 采用实际高度还是对齐高度计算 Y 平面字节数，并经 `dec_chan_set_image_param`、`fsp_set_image` 写入图像描述符 `+0x294`。原厂 `pdt_get_y8b_data` 的 `0xc081d6b0` 和 `pdt_get_c8b_data` 的 `0xc081d7a4` 是按行 stride 的 CPU 读取分支。两条色度读取分支都按 VU 取样。[原厂反编译](artifacts/vpu-reverse-factory/arm32-result.txt) · [布局指令](artifacts/vpu-reverse-factory/vdh-layout-extra.txt)

| 字段 | 32×32 输出配置 |
| --- | --- |
| `pub[3]` | tiled 为 `0x00030000`，设置原厂 2D 请求后为 `0x20030000` |
| `pub[9]/[10]/[12]/[20]` | 输出基址除以 16、256 B stride、Y 平面长度、YUV 结束地址除以 16 |
| `core+0x5c0/0x5c4/0x5c8/0x620` | 原厂在任务 `+0x98` 非零时写图片描述符 `+0x20` 左移 16 位的值。本向量无裁剪，保持为 0 |
| `core+0x5cc..0x610` | 三组宽高 `0x00200020`、Y 平面长度、压缩及扩展平面参数、stride |
| `core+0x614/0x618/0x61c` | 当前帧 YUV 结束地址、压缩区长度参数、附加平面地址。8-bit 无压缩模式下后者等于输出基址 |
| VDH `+0x14` 与任务槽 `+0x04` | `dec_hal_write_reg` 分别从任务 `+0x88` 和 `+0x8a`、`+0x0e` 取值。两个寄存器写入均无 `+0x98` 或已识别的 UV 顺序字段 |

板测沿用同一 CAVLC IDR，先用 tiled 模式的 256 B stride 和 16,384 B Y 平面，再设置 bit 29 和 8,192 B Y 平面。两次均各完成一次 PXP IRQ，SMMU 零故障。两次按线性 stride 回读都只匹配 384/1,536 B，按 64 B×16 的 Y tile 与 64 B×8 的 VU tile 回读均匹配 1,536/1,536 B。已确认这组原厂线性请求与缓冲参数在本板 VDH 路径上仍直出 tiled VU。独立 NV12 顺序控制及其他使线性直出的配置尚无板测证据。[最终串口](logs/431-vpu-h264-final-serial.txt) · [像素校验](logs/432-vpu-h264-pixel-check.txt)

### CABAC 和参考帧

固定向量为 32×32、8-bit、Main Profile，IDR 和 P 各有一个 slice，P 只有一个 L0 参考。输入第 0 帧为原 CAVLC 参考图像，第 1 帧的右下宏块在 Y、U、V 上各增加 36 并限制到 235。FFmpeg/libx264 使用 `-preset medium -qp 24 -x264-params cabac=1:bframes=0:ref=1:keyint=30:scenecut=0:weightp=0:threads=1:slices=1`，用 `filter_units=remove_types=6` 移除 SEI。编码器报告 P 帧四个宏块中的三个为 P-skip，软件解码结果相差 400 B。[码流](artifacts/vpu-reverse-factory/h264-ip-cabac-32x32.264) · [头字段](artifacts/vpu-reverse-factory/h264-ip-cabac-32x32-trace.txt) · [编码器参数](https://ffmpeg.org/ffmpeg-codecs.html#libx264_002c-libx264rgb)

| 原厂路径 | 本次写入 |
| --- | --- |
| `hal_h264_write_pub_msg` | `pub[6]` 指向两帧共用的 Annex B DMA 缓冲，`pub[23]` 指向当前帧独立 PMV，`pub[25]` 指向 CABAC 初始化表 |
| `hal_h264_write_pic_msg` | `pic[0][27]` 置位，`pic[2]` 的当前 POC 分别为 0 和 2。P 帧 `pic[10]` 为 `OUTPUT_0` 的 IOVA 除以 16，`pic[27]/[28]` 为参考 IDR 的 top/bottom POC 0。core 的三组参考地址从 `+0x500/+0x540/+0x580` 开始 |
| `hal_h264_write_slice_msg` | IDR/P 的 CABAC 数据分别从 Annex B 字节 41、67 开始。slice `+0x18[31:26]` 为 QP 21/24，`[25:24]` 为 CABAC init 0。P slice `+0x1c` 的类型为 1，L0 数量减一为 0，`+0x30/+0x50` 选逐行 DPB 槽 0 |
| ColMV / PMV | 保留 `PMV_0`，第 1 帧写 `PMV_1`。原厂仅在 B slice 分支给 `slice+0x20/+0x24` 填 ColMV 读取地址，逐行 P slice 使用空间 MV 预测 |
| VDH 槽和 SMMU | 第 0 帧使用 `VDH+0x100`，第 1 帧使用 `VDH+0x120`。两个任务独立消息和回写区，引用面和 PMV 保持驻留。固定页表在提交前一次性写好，帧间无需重设页表、刷新 TLB 或复位 PXP |

首轮复用 `VDH+0x100` 时第 1 帧超时，UP_MSG 写回区保留填充值。原厂 `dec_dev_acquire_reg`（`0xc07773ac–0xc07773bc`）按槽数循环推进。换到下一槽后，PXP 中断状态从 `0x00020000`（槽 0）到 `0x01020000`（槽 1），INTID 169 在一次双帧命令中增加 2，INTID 181 保持 0。[复用超时](logs/425-vpu-h264-cabac-serial.txt) · [切换槽验证](logs/427-vpu-h264-cabac-slots-serial.txt)

| 帧 | PXP 完成 | SMMU 故障 | I420 匹配 | 板端和参考 SHA256 |
| --- | ---: | ---: | ---: | --- |
| 0 CABAC IDR | 1 | 0 | 1,536/1,536 B | `ffcb5e22013d3f42d9260f7c1e54f2bb082876dda77c7231c6088f331139f186` |
| 1 CABAC P | 1 | 0 | 1,536/1,536 B | `b784494112741557444cec4efc50d15fefe5e7949b14e44625836982eeacbf2c` |

生成头由 `python scripts/vpu_h264_data.py artifacts/vpu-reverse-factory src/hi3798mv320-vpu-h264-data.h` 生成。通过 `scripts/remote.ps1` 同步两个头至构建服务器 `inputs/`，用第 6 节的命令将目标改为 `arm64-7.3.5-vpu-h264-poc`。最终 `boot.img` SHA256 为 `a9cf3c034c3c23591c1971de109f99755317f0e4093a27c9d570453f98d7485c`，U-Boot 实测 TFTP CRC32 为 `46e656d6`。[构建及镜像校验](logs/430-vpu-h264-final-build.txt) · [RAM 镜像](artifacts/arm64-7.3.5-vpu-h264-final/boot.img)

从 COM4 以 115200 8N1 独占连接，经 TFTP 把 `boot.img` 传到 `0x01fdffc0`，用 U-Boot `crc32` 校验后执行 `bootm`。下列 sysfs 读回为十六进制 I420，每行 3,072 个十六进制字符。其中 `linear_frame` 使用行 stride 读取，`frame` 与 `frame1` 使用 tile 寻址。[最终串口](logs/431-vpu-h264-final-serial.txt) · [IDR 参考](artifacts/vpu-reverse-factory/h264-ip-cabac-32x32-frame0.i420) · [P 帧参考](artifacts/vpu-reverse-factory/h264-ip-cabac-32x32-frame1.i420)

```sh
cd /sys/devices/platform/soc/f8c30000.video-decoder
echo cavlc > decode
cat frame linear_frame
echo probe-linear > decode
cat frame linear_frame
echo 1 > decode
cat frame frame1
grep hg680kd /proc/interrupts
```

同一次 RAM 启动内复跑双帧命令，INTID 169 从 4 增至 6，INTID 181 保持 0，两个输出哈希与首轮一致。USB 未挂载，`mmcblk0/ro=1`，U-Boot 环境变量仅在本次会话内设置。[复跑串口](artifacts/arm64-7.3.5-vpu-h264-final/repeat-serial.txt) · [像素校验](logs/432-vpu-h264-pixel-check.txt)

## 9. Linux 7.3.5 V4L2 Stateless 驱动

`arm64-7.3.5-vdec` 使用 `hi3798mv320-vdec` 平台驱动，注册 Media Controller 和 V4L2 多平面 M2M 设备。OUTPUT 接收带 Annex B 起始码的 `V4L2_PIX_FMT_H264_SLICE`，每个 Media Request 绑定一个 OUTPUT 缓冲。CAPTURE 接收由驱动解排布的线性 `V4L2_PIX_FMT_NV21`，一块缓冲依次存放 Y、VU。驱动按 CAPTURE 时间戳查找用户态提供的 DPB 引用。一个画面含多片时，请求使用 `V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF` 保留同一 CAPTURE 缓冲，最后一片提交完整的 VDH slice 链。相关 UAPI 见 [Linux Stateless Decoder Interface](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html)。

OUTPUT 和 CAPTURE 都使用 VB2 dma-contig。驱动为 CAPTURE 索引单独保留 tiled 解码面和 PMV 区，亮度 stride 对齐 256 B，高度对齐 64 行。运动向量区域按宏块尺寸计算。硬件缓冲经 4 KiB 页的私有 SMMU 表映射。PXP 停机并应答复位后重载表基址，再交还待复用的 IOVA。板测记录显示 `0x10000000` 先后映射到不同的 DMA 地址，解码结果保持一致。PXP 在 `VDH+0x100`、`VDH+0x120` 完成两次任务后复位游标，DPB 表面和 PMV 保留在 DMA 内存中。

### 构建与复现

通过 `scripts/remote.ps1` 把驱动、DTS、补丁和 C 测试程序同步到 `/home/hedc/hg680kd-mainline/inputs`，把更新的 `build_arm64.sh` 和 `pack_arm64.py` 同步到 `scripts/`。驱动输入包括 `hi3798mv320-vdec.c`、`hi3798mv320-vdec-neon.S`、CABAC 头及 `0012`、`0013` 补丁。增加 `VDEC_TEST=1` 会将测试程序和 fixtures 一并放入 RAM initramfs。

```bash
cd /home/hedc/hg680kd-mainline
JOBS=36 bash scripts/build.sh full "$PWD" "$PWD/inputs" arm64-7.3.5-vdec
cd out/release-arm64-7.3.5-vdec
sha256sum -c BOOT-SHA256SUMS
```

用下面的命令准备两个基本测试向量，并将生成的 `.txt`、`.264` 放入远端 `inputs/`。软件参考 `.nv21` 留在主机用于比较。构建测试镜像时传入 `VDEC_TEST=1`。通过 `VDEC_FIXTURES` 可以指定其他同格式样本。

```powershell
$out = 'artifacts/arm64-7.3.5-vdec'
New-Item -ItemType Directory -Force $out | Out-Null
python scripts/vdec_request_fixture.py artifacts/vpu-reverse-factory/h264-ip-cabac-32x32.264 "$out/ip32"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=128x128:rate=4 -frames:v 5 -c:v libx264 -threads 1 -profile:v main -x264-params 'ref=1:bframes=0:weightp=0:slices=1:keyint=30:scenecut=0' -an -f h264 "$out/input128.264"
python scripts/vdec_request_fixture.py "$out/input128.264" "$out/ip128"
```

COM4 以 115200 8N1 独占打开。U-Boot 通过 TFTP 将镜像放入 `0x01fdffc0`，板端 CRC32 与本地一致后运行 `bootm 0x1fdffc0`。板上只读准备固件，测试输出写入 RAM 中的 `/tmp`。eMMC 保持 `ro=1`，U 盘未挂载，OTP 与持久 U-Boot 环境保持原样。

```sh
vdec-request /dev/video0 /dev/media0 /etc/vdec-test/ip32.txt /etc/vdec-test/ip32.264 /tmp/ip32.nv21
vdec-request /dev/video0 /dev/media0 /etc/vdec-test/ip128.txt /etc/vdec-test/ip128.264 /tmp/ip128.nv21
grep hi3798mv320-vdec /proc/interrupts
sha256sum /tmp/ip32.nv21 /tmp/ip128.nv21
```

驱动的 CABAC 初始化表可用 `python scripts/vdec_cabac_table.py artifacts/vpu-reverse-factory src/hi3798mv320-vdec-cabac.h` 从原厂内核重新生成。生成器在提取前核对原厂内核 SHA256。

### 初版实机结果

最终 RAM 镜像 SHA256 为 `07a9b999c0061c2d0bd534676498e0b5150efc78f1ab881dcfff952fe83d9a50`，U-Boot TFTP 回读 CRC32 为 `59365333`。板端注册 `/dev/video0`、`/dev/media0`。正向测试完成 26 帧，INTID 169 从 0 增到 26，INTID 181 为 0。负向测试先解码 B 帧样本前两张参考图，PXP 总数增到 28，SMMU 仍为 0。

| 样本 | 通过帧数 | 软件参考 |
|---|---:|---|
| 32×32 Main CABAC IDR/P | 2 | NV21 与 I420 两种布局逐字节相等 |
| 32×32 CAVLC IDR | 1 | NV21 逐字节相等 |
| 128×128 CABAC 单片 | 5 | NV21 逐字节相等 |
| 128×128 CABAC 双片 | 4 | NV21 逐字节相等 |
| 128×128 CABAC 双片并使用 `V4L2_DEC_CMD_FLUSH` | 4 | NV21 逐帧 SHA256 相等 |
| 256×512、512×256、640×480、640×720、960×544 CABAC IDR/P | 各 2 | NV21 逐帧 SHA256 相等 |

初版枚举宽 32–960、高 32–720，步进 16，支持逐行 8-bit 4:2:0 I/P。1024×512、1280×256 和 1280×720 探测首帧均报 PXP `0x60000`，SMMU 故障为 0。128×128 空间和时间直接预测 B 帧都完成硬件任务，输出与参考存在像素差异，因此初版驱动拒绝 B 帧。

[构建日志](logs/433-vdec-build.txt) · [COM4 TFTP 日志](logs/434-vdec-tftp.txt) · [逐帧 SHA256 对照](logs/435-vdec-pixels.txt)

### 720p、1080p 和 B 帧修复

**PXP `0x60000`** 同时置位 PXP 状态 bit 17（任务完成）和 bit 18（解码错误）。原厂 `dec_hal_read_int_info` 分开提取这两位，`dec_hal_read_frm_msg` 把 UP_MSG 首字 bit 31 作为帧错误标志。实测 1280×720 首帧的帧和首 slice 回写状态分别为 `0x94210000`、`0x94230000`，`UP[2]` 为 `0x2f`，PXP `+0x20/+0x24/+0x28` 和 SMMU 状态均为零。驱动把包含 `00 00 03` 的 Annex B 字节序列直接交给了期望 RBSP 的熵解码器，导致其在防竞争字节处读错。驱动现将每个 NAL 转成 RBSP，再按其实际长度和 RBSP 位偏移提交 slice。保留原来的 stride、`pub[21]` 和工作缓冲配置，只改变这项输入处理，1024×512、1280×256 和 1280×720 即逐帧通过。原厂尺寸字段也能容纳 1920×1088，逐行路径在宏块宽度超过 256 时才请求外部行缓冲。[错误状态与 RBSP 对照](logs/438-vdec-hd-b-pixels.txt)

**B 帧** 的 VDH slice 第 7 字原先把 SPS `direct_8x8_inference_flag` 直接写入 bit 14。只清除该位，两种既有逐行样本即从各 2/3 帧提高到各 3/3 帧匹配。当前帧和参考帧 PMV 地址、DPB 槽、顶底场 POC、L0/L1 以及控制空间直接预测的 bit 15 在该对照中用同样的值。另一个 720p 双 slice 时间直接预测样本也通过 3/3 帧。[B 帧逐位对照和逐帧 SHA256](logs/438-vdec-hd-b-pixels.txt)

当前枚举为宽 32–1920、高 32–1088，步进 16，支持逐行 8-bit 4:2:0 I/P/B。SPS 指定 1080 行可视高度时，CAPTURE 返回 1088 行编码面，应用按 SPS 裁剪。以下向量使用现有的测试程序和 FFmpeg 生成器。[测试程序](src/vdec-request-test.c) · [样本生成器](scripts/vdec_request_fixture.py)

```powershell
$out = 'artifacts/arm64-7.3.5-vdec'
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=4 -frames:v 4 -c:v libx264 -threads 1 -profile:v high -x264-params 'ref=1:bframes=0:weightp=0:slices=4:keyint=30:scenecut=0' -an -f h264 "$out/input1080-crop.264"
python scripts/vdec_request_fixture.py "$out/input1080-crop.264" "$out/ip1080crop"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=128x128:rate=4,fade=t=in:st=0:d=1.0' -frames:v 5 -c:v libx264 -threads 1 -profile:v main -x264-params 'ref=1:bframes=0:weightp=2:slices=1:keyint=30:scenecut=0' -an -f h264 "$out/input128w.264"
python scripts/vdec_request_fixture.py "$out/input128w.264" "$out/ip128w"
```

最终 RAM 镜像 SHA256 为 `c1048f4dd72b7cae84a11f7c2dd88edcb8b8990372c4f59bc29c91b67c22ae90`，TFTP 到 `0x01fdffc0` 后 U-Boot 回读 CRC32 为 `0574dfc3`。从启动到测试完成，PXP INTID 169 从 0 增至 52，SMMU INTID 181 始终为 0。U 盘未挂载，`mmcblk0/ro=1`。[构建日志](logs/436-vdec-hd-b-build.txt) · [COM4 TFTP 日志](logs/437-vdec-hd-b-tftp.txt) · [逐帧 SHA256 对照](logs/438-vdec-hd-b-pixels.txt)

## 10. Linux 7.3.5 V4L2 Stateless HEVC Main 8-bit

`hi3798mv320-vdec` 按 OUTPUT 格式选取 `vdec_codec_ops`。后端决定工作区分配、PMV 大小及请求消息，每个上下文持有各自的 CAPTURE 参考图。时钟、复位、私有 SMMU 动态映射、VDH 两槽、PXP IRQ、VB2 和 NV21 解排布共用。HEVC OUTPUT 接收 `V4L2_PIX_FMT_HEVC_SLICE` 的 Annex B VCL NAL，每个请求一片。SPS 可设在当前控件值，首片请求带 PPS、DECODE_PARAMS 和有效的 Scaling Matrix，后续片可用 HOLD 保留 CAPTURE 缓冲。应用按 SPS conformance window 裁剪。标准控件的位流范围和偏移采用 Annex B 输入的约定。[驱动](src/hi3798mv320-vdec.c) · [Linux Stateless Codec Controls](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-codec-stateless.html) · [GStreamer HEVC 控件填充](https://raw.githubusercontent.com/GStreamer/gstreamer/main/subprojects/gst-plugins-bad/sys/v4l2codecs/gstv4l2codech265dec.c)

### 原厂消息核对

`arm32-result.txt` 已覆盖 `dec_hal_write_pub_msg`（`c077b140`）、`dec_hal_write_core_msg`（`c077b3e0`）及任务区分配。原始导出筛选偏向 H.264。用同一份原厂内核、kallsyms 和 IDA 数据库补导 `hevc_*`、`dps_h265_*`、`hal_h265_*`。在 `s_dec_hal_ops`（`c176bbf0`）中，第二项的第一个回调是 `hal_h265_write_msg`（`c07c6b8c`），末字为原厂软件协议号 `0x11`。[原始导出](artifacts/vpu-reverse-factory/arm32-result.txt) · [HEVC 补充导出](artifacts/vpu-reverse-factory/arm32-hevc-result.txt)

| 区域 | 字段及来源 |
|---|---|
| `pub[2..3]` | `hal_h265_write_pub_msg`（`c07c5a4c`）写入正常解码高字节 `0x01`，启用 Scaling List 时再置 bit 30。低 20 位为 CTU 总数减一。硬件协议号 `13` 写在 `pub[3]` 的低字节。8-bit、非压缩且有 PMV 的常规任务使用 `0x0003c00d`。 |
| 尺寸 | `log2_ctu = log2_min_luma_coding_block_size_minus3 + 3 + log2_diff_max_min_luma_coding_block_size`。CTU 网格宽高各按 CTU 尺寸向上取整，减一后存入 `pub[31]` 低、高半字。`pic[0]` 中宽高减一分别从 bit 0、9 起。实际编码像素宽高写入 `pic[54]` 与 `core+0x5cc`。 |
| `pic[0..3]`、`pic[60..61]` | `hal_h265_write_pic_msg`（`c07c5bf4`）打包 SPS/PPS。含 AMP、SAO、temporal MVP、编码及变换块尺寸、变换层级、加权预测、色度 QP、CU QP delta、WPP、Tile 和环路滤波控制。当前 POC 写入 `pic[1]`。 |
| Scaling List | `dps_h265_write_qmatrix_8x8` 与 `dps_h265_write_qmatrix_16x16` 将光栅顺序改为按 4 个系数打包。快照从 `pic+0x200` 起，偏移 `0x000`、`0x180`、`0x300`、`0x380` 分别为 8×8、16×16、32×32、4×4，`0x3e0` 起为 8 个 DC 系数。 |
| DPB / RPS | `pic[8..23]` 是 16 个参考图地址，`pic[26..41]` 是对应 POC。参考地址与 POC 顺序一致，V4L2 以 CAPTURE 时间戳查找图像。原厂 `hal_h265_make_slice_ref`（`c07c6ac4`）将两个列表转为硬件 DPB 索引。每片的长期参考位图位于 `slice[13..14]`，4-bit 索引位于 `slice[15..18]`。共位参考索引在 `slice[7]` 的高 4 位，`slice[23]` 指向对应 CAPTURE 的 PMV。 |
| Tile 信息 | `pub[32]` 与 `pic[61]` 携带 Tile 列数和行数减一。`pic[55]` 指向任务基址 `+0x33c00`。前两个 512 B 分别映射 X/Y 方向的 16 像素单位到 Tile 索引，后接 20 对列边界和 22 对行边界，每对为两个 LE16 CTU 索引，结束索引包含在内。 |
| Slice / core | `core[28]` 指向首片，`core[32]` 指向首片 UP，`core[36]` 为片数。`hal_h265_write_slice_msg`（`c07c5f30`）在 `slice[0..5]` 写入两个环形流段的 128-bit 单位偏移、段内 bit 偏移及 bit 数。驱动使用连续工作区，因此只用第一段。 `slice[8..10]` 包含 CTU 起点的光栅索引、X/Y 坐标和 Tile 顺序索引，`slice[43..44]` 是对应的结束索引，`slice[63]` 是下一条消息的地址，最后一条为零。 |

地址字段以 16 B 为单位。任务区的 core、picture、首条 slice 分别位于 `+0x100`、`+0x800`、`+0xe00`。HEVC 原厂允许 200 条 0x400 B 消息，恰好到 `+0x32e00` 的 picture UP 区。首条 slice UP 位于 `+0x32e80`。WPP 的 720p 板测使用单个连续 RBSP 流段。

### 缓冲布局与 RBSP

`hal_h265_fill_ext_mem`（`c07c57f8`）以 1024 B 对齐工作区基址并返回包含对齐补差的 `0x38100` B 大小。用于行缓冲和中间数据的原厂地址槽如下。区间大小按相邻子区起点和总长度核对，区间用途尚未逐个定位到硬件单元。

| `pub[]` | 相对于 HEVC `ext_work` | 区间大小 |
|---:|---:|---:|
| 21 | `0x00000` | `0x1000` |
| 22 | `0x01000` | `0x3000` |
| 26 | `0x04000` | `0x3000` |
| 24 | `0x07000` | `0x2800` |
| 30 | `0x09800` | `0xb100` |
| 28 | `0x14900` | `0xb000` |
| 29 | `0x1f900` | `0x3800` |
| 27 | `0x23100` | `0x15000` |

`pub[21]` 在当前最大宽度下跟随原厂窄画面分支清零。`pub[25]` 指向单独的 HEVC CABAC 初始化表。从原厂 `c10b1220` 取三组初始化值并生成 928 B 的有符号 `(m,n)` 对。表生成命令与第 9 节相同。PMV 随解码面保留，尺寸按原厂 `pdt_get_slot_wh` / `pdt_get_pmv_size` 使用 `ALIGN(width,64) * ALIGN(height,64) / 4 + 256`。Y stride 按 256 B 对齐，Y 区占 `stride * ALIGN(height,64)`，VU 紧随其后且为 Y 区一半。驱动将 PMV 起点页对齐，Y 和 VU 用共用的 tiled 解排布写为紧凑 NV21。

OUTPUT 保留 Annex B 起始码与 2 B NAL 头。驱动复制 VCL NAL 时剥离 `00 00 03` 中的防竞争字节，按 RBSP 的 CABAC 数据位置填充消息。GStreamer 的 `data_byte_offset` 含起始码与 NAL 头，并已扣除 slice header 内的防竞争字节。720p 对照实验的 VCL 含 15 个 `00 00 03`，原样送 EBSP 时首帧报 PXP `0x60000`，剥离后 3 帧像素哈希全部一致。两轮 SMMU 故障数均为 0。[EBSP/RBSP 对照串口](artifacts/arm64-7.3.5-vdec-hevc/diag/serial.txt)

### 构建与 RAM 板测

测试生成器读 FFmpeg 的 `trace_headers`，产出 `.txt` 请求参数、`.265` Annex B VCL 和 `.nv21` 软件参考。它按 POC 将 FFmpeg 的显示顺序转换到请求的解码顺序。带 conformance window 的样本另生成 `.visible.nv21`。下面创建 64×64 I+P 样本，帧数改为 1 即为 IDR 样本。[样本生成器](scripts/vdec_hevc_fixture.py) · [Media Request 测试程序](src/vdec-request-test.c)

```powershell
$out = 'artifacts/arm64-7.3.5-vdec-hevc'
New-Item -ItemType Directory -Force $out | Out-Null
$x265 = 'pools=1:frame-threads=1:wpp=0:repeat-headers=1:weightp=0:weightb=0:ref=1:keyint=30:scenecut=0:open-gop=0:b-pyramid=0'
ffmpeg -y -f lavfi -i 'testsrc2=size=64x64:rate=4' -frames:v 2 -pix_fmt yuv420p -c:v libx265 -profile:v main -x265-params "${x265}:ctu=32:bframes=0" -f hevc "$out/input-h265ip64c32.265"
python scripts/vdec_hevc_fixture.py "$out/input-h265ip64c32.265" "$out/h265ip64c32"
```

1920×1080 源可用 `-vf pad=1920:1088:0:0` 编码为 1088 行，再用 `-bsf:v hevc_metadata=crop_bottom=8` 标出 1080 行可视区。Tile 样本用 libkvazaar 的 `tiles=2x2`，在第二个样本加入 `slices=tiles`。[Kvazaar 编码参数](https://github.com/ultravideo/kvazaar#command-line-options)

通过 `scripts/remote.ps1` 同步驱动、NEON 汇编、CABAC 头、测试程序、`0012` 和 `0013` 补丁以及 fixture 的 `.txt` 与码流到 `inputs/`，同步 `build_arm64.sh` 到 `scripts/`。选择要嵌入的样本后打包并核对远端 SHA256。
已有 Linux 源树若应用过旧版 `0012`，将 `drivers/media/platform/Kconfig` 中该选项的帮助行同步至新版措辞，构建脚本即可识别补丁已应用。

```bash
cd /home/hedc/hg680kd-mainline
JOBS=36 VDEC_TEST=1 VDEC_FIXTURES="h265ip64c32 h265crop1080 ip32 ip1080crop ip128b" \
  bash scripts/build.sh full "$PWD" "$PWD/inputs" arm64-7.3.5-vdec
cd out/release-arm64-7.3.5-vdec
sha256sum -c BOOT-SHA256SUMS
```

下载 `boot.img` 后，用主机测试脚本独占 COM4。脚本在 U-Boot 中进行 TFTP、核对 CRC32，再从 RAM 执行 `bootm`。Linux 输出写进 RAM 的 `/tmp`。脚本从板端逐帧读取 SHA256 并与主机 FFmpeg 参考比对。

```powershell
python scripts/vdec_ram_test.py --image "$out/final/boot.img" `
  --log "$out/final/serial.txt" --crop h265crop1080=1080 `
  "$out/h265ip64c32" "$out/h265crop1080"
```

### 实机结果

在同一次 COM4 115200 8N1 TFTP RAM 启动中，HEVC 35 帧和 H.264 9 帧的 NV21 哈希逐帧与 FFmpeg 一致。1088 行编码面及其 1080 行可视区分别核对，裁剪比对不重复计入解码帧总数。[构建日志](logs/439-vdec-hevc-build.txt) · [COM4 日志](logs/440-vdec-hevc-tftp.txt) · [逐帧 SHA256](logs/441-vdec-hevc-pixels.txt)

| 格式 | 样本 | 匹配 |
|---|---|---:|
| HEVC | 64×64 IDR，32×32 CTU | 1/1 |
| HEVC | 64×64 I+P，32×32 CTU | 2/2 |
| HEVC | 128×128，含 B 帧，解码顺序 POC 0、2、1、3 | 4/4 |
| HEVC | 1280×720 I/P | 3/3 |
| HEVC | 1920×1080 I/P | 3/3 |
| HEVC | 1920×1088 编码面及 1920×1080 裁剪 | 各 3/3 |
| HEVC | 128×128 默认 Scaling List | 3/3 |
| HEVC | 128×128 加权 P | 6/6 |
| HEVC | 1280×720 WPP | 3/3 |
| HEVC | 256×256 双 slice | 3/3 |
| HEVC | 256×256 2×2 Tile，单 slice / 分 Tile 多 slice | 各 2/2 |
| H.264 | 32×32 Main CABAC I+P | 2/2 |
| H.264 | 1920×1088 编码面及 1920×1080 裁剪 | 各 4/4 |
| H.264 | 128×128，含 B 帧 | 3/3 |

镜像 SHA256 为 `b13e4218b67343a10fce90ee0229ebd7cabdff5177d1027edfca080366044fb2`。U-Boot 从 `0x01fdffc0` 读回 16,644,168 B，CRC32 为 `9690610d`。PXP INTID 169 从 0 增至 44，SMMU INTID 181 全程为 0。U 盘未挂载，`mmcblk0/ro=1`。U-Boot 只读取原厂 ATF 并设置临时会话变量，镜像通过 TFTP 写入 RAM。[最终 boot.img](artifacts/arm64-7.3.5-vdec-hevc/final/boot.img)

单 CTU 行的 HEVC 图像在探测镜像中有 slice UP 更新，但 PXP 不完成。实测包括使用 64×64 CTU 的 64×64 与 128×64，以及使用 32×32 CTU 的 128×32。最终驱动在硬件提交前以 `-EOPNOTSUPP` 拒绝此组合，负向测试中两个中断计数均未增加。本节的板测使用逐行 8-bit 4:2:0。长期参考地址和索引已按原厂布局映射，本轮的 I/P/B 向量验证了短期参考。[单 CTU 行日志](logs/441-vdec-hevc-pixels.txt)

| 样本 | NV21 帧 SHA256 匹配 |
| --- | ---: |
| 32×32 CABAC IDR/P 和 CAVLC IDR | 3/3 |
| 128×128 单片、双片、双片 Flush | 13/13 |
| 960×544、1024×512、1280×256 | 6/6 |
| 1280×720 单片、纯色、八片 | 12/12 |
| 1080p 四片，1920×1088 编码面及 1920×1080 可视区域 | 各 4/4 |
| 128×128 空间与时间直接预测 B 帧、720p 双片 B 帧 | 各 3/3 |
| 128×128 加权 P，含非默认亮度与色度权值和偏移 | 5/5 |

## 11. Linux 7.3.5 UHD 与 HEVC Main10

### 尺寸、行缓冲与内存容量

H.264 和 HEVC 的 OUTPUT 与 CAPTURE 最大编码面为 4096×2176，分别按 16 和 8 像素步进。原厂 `pdt_get_dec_cap`（`c081e3d4`）给出 4096×2304 的通用尺寸。4096×2176 的 H.264 宏块网格为 256×136，HEVC 在 16×16 CTU 下也是 256×136，在 64×64 CTU 下为 64×34。最大网格的总块数减一为 34,815，可装入 `pub[2]` 的低 20 位。横纵计数减一分别为 255、135，可装入 H.264 `pic[0]` 的宽高字段与 HEVC `pic[0]` 的两个 9-bit 字段。编码宽高及并排的 CTU、宏块坐标可装入 `core+0x5cc`、`pub[31]` 等处的半字。HEVC 的两个 512 B Tile 查找表按 16 像素索引，最大使用 256 项和 136 项。3840×2160 使用 3840 B 主面 stride 与 2176 行分配高度。[原厂导出](artifacts/vpu-reverse-factory/arm32-result.txt) · [HEVC 导出](artifacts/vpu-reverse-factory/arm32-hevc-result.txt)

`hal_h264_fill_ext_mem`（`c07a8c40`）在 1 KiB 对齐的基址上分配 `0x21000` B，子区起点为 `0x0000`、`0x6000`、`0xe000` 和 `0x1e000`。逐行 H.264 的宽度超过 256 个宏块才启用 `pub[21]`。HEVC 的 `dec_hal_calc_mem_param`（`c077bc40`）把像素宽度 ≤4096 的标志送入 `hal_h265_write_pub_msg`，该范围内 `pub[21]` 为零。HEVC 沿用第 10 节的固定 `0x38100` B 工作区，原厂 `hal_h265_fill_ext_mem` 的分配没有位深分支。H.264 和 HEVC 的 PMV / ColMV 分别按 `ALIGN(w,64) * ALIGN(h,32) / 4 + 256` 与 `ALIGN(w,64) * ALIGN(h,64) / 4 + 256` 分配。Main10 保持相同的 PMV 容量，随扩展后的参考面移动 PMV 起点。[原厂导出](artifacts/vpu-reverse-factory/arm32-result.txt) · [HEVC 导出](artifacts/vpu-reverse-factory/arm32-hevc-result.txt)

私有 SMMU 的 IOVA 池为 1 GiB，地址范围是 `0x10000000–0x4fffffff`，4 MiB 平坦页表使用 4 KiB 页。最大编码面的一个 8-bit 或 Main10 硬件参考面分别占 15,605,760 B 或 18,948,096 B，均含 PMV 和页对齐。17 个最大 Main10 面共约 307 MiB，另有 4 MiB 页表和最多约 17 MiB 任务区。此前 RAM 内核的 CMA 为 0。本版仅对 VDEC RAM 构建启用 CMA 和 DMA CMA，并在强制内核命令行加入 `cma=384M`。VB2 OUTPUT/CAPTURE 保留 MMAP 接口，软件读写的线性和码流缓冲改用 `vb2_vmalloc_memops`，硬件参考面、任务区和页表仍用连续 DMA。

### Main10 消息与原厂布局

`hevc_process_sps`（`c07af4c4`、`c07af5c4`、`c07af6c8` 附近）读取两个位深减 8 的 Exp-Golomb 值，将实际位深写入 SPS `+0x58/+0x60`，并在 `+0x5c/+0x64` 存入 `6 × (bit_depth - 8)`。`dps_h265_make_dps_pic` 把这些字段从 SPS `+0x38` 开始的结构复制到 DPS。`hal_h265_write_pic_msg` 将亮度、色度实际位深放入 `pic[3]` 的低两个半字节，对应的 QP 偏移从 bit 16、22 开始。逐行 4:2:0、关闭 PCM 时，Main 8-bit 为 `0x00000088`，Main10 为 `0x030c00aa`。`slice[6]` 的 bit 19–25 为 7-bit QP 字段，Main10 的 QP 下界为 −12。[HEVC 导出](artifacts/vpu-reverse-factory/arm32-hevc-result.txt)

原厂 `pdt_get_y8b_data` / `pdt_get_ynb_data` 以及 `pdt_get_c8b_data` / `pdt_get_cnb_data` 读取两套平面，再用 `(MSB8 << 2) | LSB2` 恢复采样。非压缩 Main10 的 8-bit 主面保持 Y、VU 的 Tiled 布局，随后是独立的 2-bit Y、VU 平面。每个低位平面用 64 个采样 × 2 行的小块，四个采样按低位优先装进一字节。令 `S = ALIGN(w,256)`、`Y = S × ALIGN(h,64)`，则主面总大小为 `3Y/2`，低位 Y 和 VU 分别为 `Y/4`、`Y/8`，低位 stride 为 `S/4`。低位平面内坐标 `(x,y)` 的字节偏移为 `(y & ~1) × (S/4) + (x/64) × 32 + (y%2) × 16 + (x%64)/4`，其中色度的 `x` 计入交织的 V 与 U 两个采样。[原厂导出](artifacts/vpu-reverse-factory/arm32-result.txt)

| 消息字段 | 非压缩 Main10 写入值 |
| --- | --- |
| `pub[3]`、`pub[9..12]` | HEVC 协议和原有非压缩控制保持 `0x0003c00d`，起址、主面 stride 和 Y 区大小仍用 8-bit 主面参数 |
| `pub[14..16]` | `S/4`、`3Y/2`、`Y/4`，分别是低位 stride、相对参考面基址的低位 Y 偏移、低位 VU 相对低位 Y 的偏移 |
| `pub[20]`、`pub[23]` | 四个平面结束地址、当前面 PMV 地址 |
| `core+0x500/0x540/0x580` | 16 槽 DPB 的主面基址、主面基址加 `3Y/2`、低位 Y 基址 |
| `core+0x5f0/0x608` | 每组的低位 VU 偏移 `Y/4` 与低位 stride `S/4` |
| `core+0x614/0x61c` | 当前面分别填主面基址加 `3Y/2` 与低位 Y 基址 |
| `pic[8..23]`、`slice[23]` | DPB 仍以各主面基址索引，ColMV 指向相应参考面的 PMV |

通用低位计算出自 `dec_hal_calc_dec_mem_param`（`c077b9bc`），任务字段由 `dec_hal_write_pub_msg` 和 `hal_h265_write_msg` 写入。表中基址按 VDH 的 16 B 单位传递，stride 和相对偏移保留字节单位。原厂在某些 UHD 配置下会选压缩参考面，主线使用上表的非压缩路径。[原厂导出](artifacts/vpu-reverse-factory/arm32-result.txt) · [HEVC 导出](artifacts/vpu-reverse-factory/arm32-hevc-result.txt)

应用先设置 HEVC OUTPUT，再用当前 SPS 选择 CAPTURE 默认格式，也可显式协商 CAPTURE。SPS 的 luma 和 chroma 位深必须同时为 8 或同时为 10。10-bit CAPTURE 用单缓冲 P010，亮度后接交织 UV，每个 LE16 字的高 10 位包含采样。8-bit CAPTURE 继续输出 NV21。切换位深时应停止流并释放 CAPTURE 缓冲后重新协商。测试 manifest 的 `HEVC10` 标记选择 P010，软件参考为 FFmpeg `p010le`，含 B 帧的参考按请求解码顺序比较。[驱动](src/hi3798mv320-vdec.c) · [样本生成器](scripts/vdec_hevc_fixture.py) · [V4L2 P010](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-yuv-planar.html#v4l2-pix-fmt-p010)

### COM4 TFTP 纯 RAM 板测

初版 P010 解排布逐采样读取 coherent DMA。4K 首帧有 PXP 中断，但使用者的 3 秒 Media Request 轮询超时。把 64 B 主平面行及 16 B 低位行批量复制到栈上再拼接像素后，沿用同一超时设置通过全部阶梯。3840×2160 P010 首帧中有 7,009,383 个采样的低两位非零，整帧 SHA256 与 FFmpeg 一致。[优化前串口](artifacts/arm64-7.3.5-vdec-uhd/main10/serial.txt) · [最终串口与逐帧 SHA256](logs/443-vdec-uhd-tftp.txt)

| 样本 | CAPTURE | 与 FFmpeg 逐帧 SHA256 匹配 |
| --- | --- | ---: |
| HEVC Main，3840×2160 I/P | NV21 | 3/3 |
| H.264 High，3840×2160 四 slice I/P | NV21 | 3/3 |
| HEVC Main10，64×64 IDR / I+P，32×32 CTU | P010 | 1/1、2/2 |
| HEVC Main10，128×128，含 B 帧 | P010 | 4/4 |
| HEVC Main10，1920×1080 I/P | P010 | 3/3 |
| HEVC Main10，3840×2160 I/P | P010 | 3/3 |
| HEVC Main，1920×1080 I/P 回归 | NV21 | 3/3 |
| H.264 High，1920×1088 编码面与 1920×1080 可视区回归 | NV21 | 各 4/4 |
| HEVC Main10，4096×2176 I/P，17 个 CAPTURE 缓冲 | P010 | 17/17 |
| HEVC Main10，128×128 加权 P | P010 | 6/6 |

同一次启动共解码 49 帧，裁剪复核单独比对。PXP INTID 169 从 0 增至 49，SMMU INTID 181 全程为 0。板端报告 `CmaTotal=393216 kB`、启动后 `CmaFree=388608 kB`。4096×2176 样本一次申请并使用 17 个 CAPTURE 缓冲，验证了对应 17 个最大 Main10 硬件面的分配。

构建机为 `hedc@ArchLinux-E5-Server:/home/hedc/hg680kd-mainline`，使用 `scripts/remote.ps1` 同步驱动、测试程序和样本。若源树已应用旧版 `0012`，同步 `VIDEO_HI3798MV320_VDEC` 的 `select VIDEOBUF2_VMALLOC` 行后可复用同一构建目录。

```bash
JOBS=36 VDEC_TEST=1 \
  VDEC_FIXTURES="hevc4k h2644k hevc10idr64 hevc10ip64 hevc10b128 hevc10ip1080 hevc10ip4k h265ip1080 ip1080crop hevc10alloc hevc10wp" \
  bash scripts/build.sh full "$PWD" "$PWD/inputs" arm64-7.3.5-vdec
```

最终 `boot.img` 为 20,514,888 B，SHA256 为 `73cfb5259195b64192f5790641968e6a7e4b69c3bfb9c5092a15556a941d6f16`。COM4 115200 8N1 由单个进程独占，U-Boot 对 TFTP RAM 地址 `0x01fdffc0` 回读的 CRC32 为 `389ac230`。U-Boot 从 eMMC 只读装载 BL31，Linux 镜像和测试输出都位于 RAM。实测 USB 未挂载，`mmcblk0/ro=1`，全过程没有持久化环境变量或写入 U 盘、eMMC 与 OTP。[远程构建与校验](logs/442-vdec-uhd-build.txt) · [COM4 日志](logs/443-vdec-uhd-tftp.txt) · [最终 boot.img](artifacts/arm64-7.3.5-vdec-uhd/final/boot.img)
