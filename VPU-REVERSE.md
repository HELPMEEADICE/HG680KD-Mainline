# HG680KD Hi3798MV320 视频解码逆向

2026-11-22，烽火 HG680KD 板型 4，2 GiB LPDDR4。Linux 7.3.5 诊断镜像经 TFTP 纯 RAM 启动。VDH PXP 的复位请求和应答均可控制，SCD 和 VDH 私有 SMMU 的空闲寄存器可稳定读取。当前阶段止于探测与复位，H.264 硬解完成中断和像素回读待验证。[最终串口](logs/411-mainline-arm64-7.3.5-vpu-final-boot.txt) · [外设回归](logs/413-vpu-final-regression.txt) · [重复探测](logs/414-vpu-repeat-mmio-and-storage-hdmi.txt)

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
