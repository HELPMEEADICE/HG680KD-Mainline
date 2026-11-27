# Hi3798MV320 VDP V0 DRM/KMS 实施蓝图

目标平台为 HG680KD、Linux 7.3.5。依据 [VDEC 原生 Tiled 调研](VDEC-ZEROCOPY-STUDY.md#6-硬件证据索引) 和 [HDMI 固件交接记录](HDMI-REVERSE.md)。

## Phase 1 · 固件时序交接与 Tiled 直扫

- 建立独立的纯 RAM 测试 DT 和镜像。沿用 U-Boot 已开启的 VDP/HDMI 时钟、复位和链路，由 VDP 驱动读取活动 HD 通道并发布固定模式。首轮使用板上实有的 1080p60 或 2160p60。1080p 源可在 UHD 时序下居中 1:1 显示。继承模式容纳不下的源由 atomic check 拒绝。RAM DT 只注册原生 DRM，固件显存仍按原 DT 预留。V0 作为 primary plane，接管时关闭遮挡视频的 G0，保留固件 HDMI 输出配置。
- DT 指定 `0xf8cc0000` VDP MMIO、GIC SPI 90（INTID 122，高电平）、已由固件配置的 CRG 资源和 32 位地址直通约束。Phase 1 使用 CMA 和 VDP 专用 DMA attachment。VDH 使用自己的私有 SMMU。启动前检查活动模式与时钟状态，禁止复位正在工作的 HDMI 链路。
- 原生 CAPTURE 原型随本阶段实现。协商私有 `H8TV` / `H0TV`，一个 MMAP/CMA 内存 plane，走 `VIDIOC_EXPBUF`。采用 DMA-coherent 像素分配，VDH 私有 IOVA 紧跟映射独立 PMV/guard，导出对象止于像素面的页对齐长度。保留初始化，跳过 native 路径的 CPU detile 和每帧像素 cache sync。默认 NV21/P010 使用现有路径。
- 8-bit FB 使用 `DRM_FORMAT_NV21`，Main10 使用私有 `H0TV` fourcc。两者共享私有 modifier `0xfe00000000000320`，实验内核与工具共用头文件。约定 `S=ALIGN(width,256)`，`H=ALIGN(height,64)`，`Y=S*H`。主面 Y 和 VU pitch 均为 `S`，offset 为 `0,Y`，占 `3Y/2`。Main10 再用 pitch `S/4`、offset `3Y/2,7Y/4` 添加低 2 位 Y/VU，占 `15Y/8`。主 Y tile 是 64 B×16，主 VU 是 64 B×8。低位 tile 是 64 采样×2 行，4 个采样按低位优先打包。ADDFB2 必须用同一 GEM handle、精确的平面 offsets/pitches 和足够的对象大小。VDP 只扫描 DMA 连续、总范围可用 32 位总线地址表示的对象。
- 固件时序的 VDP IRQ 确认后接入 `drm_crtc_handle_vblank()`；在 V0 更新实际锁存后交付 flip event，再释放旧 FB。测试工具用现有 Media Request fixture 格式，先提交单帧，再跑连续 1080p / UHD Main 和 Main10，保存 IRQ、活动寄存器、CPU 及 FPS 记录。旧扫描缓冲只有在 flip 完成且超出 fixture 的 DPB 使用范围后才重排队。线性回归与相同样本的软解逐帧哈希比对。

| VDP 偏移 | 本阶段编程或核验 |
|---|---|
| `0x04/08/0c`、`0xc228/0xc2a0` | IRQ 状态、W1C 清除和 mask。HD0 VT1 使用 bit 0，阈值 12 对应后肩内的消隐中断。HD0 underflow 使用 bit 3，单独累计。 |
| `0x2000/04/18`、`0x2080–8c`、`0x8338` | V0 enable、更新触发、活动控制、1:1 视频区域和活动 Y 地址。更新位清除且活动控制、活动地址已匹配时交付翻页事件。 |
| `0x2600/04/08/0c/10`、`0x22c0/0x22d8–e8` | V0 IFIR 设为原厂模式 3 并装载其系数。ZME1 以 1:1 扫描和最近行色度扩展输出 4:2:2。1:1 路径关闭 ZME FIR。 |
| `0x4000/04/10`、`0x4080–94`、`0x4100/08`、`0x208` | V0 经 VP0 进入固件已配置的 HD0 mixer。按 `xdp_drv_set_vp0_reso`、`xdp_drv_set_vid_cbm` 和 `xdp_drv_set_reg_up` 设置 VP0 全屏范围、背景、alpha、V0 优先级并触发锁存。 |
| `0x8210/20/40` | VU 顺序位为 0。主 Y/VU 2D 取数 bypass 为 1。`data_type=2, data_fmt=1, data_width=0/1, dcmp_type=0`，复核逐行显示模式。 |
| `0x8244/48/4c` | 源分辨率、裁剪起点、取数尺寸。以有效像素尺寸编程，pitch 和偏移使用分配尺寸。 |
| `0x8250/54/58/5c`、`0x8280/84/88/8c` | 主 Y/VU、低 2 位 Y/VU 各自的高低地址。8-bit 清低位面配置，32 位 bypass 下清地址高字。 |
| `0x8290/94` | 主/低位面 pitch，低半字 Y、高半字 VU。 |
| `0xe000[0]`、`0xe2ec` | 核验 VDP 全局 bypass 与非安全 TTBR。仅 V0 平面在本阶段使用 DMA 直通。 |
| CRG `0xd8`、`0x34c`、HDMI AVI | 读取并记录固件时钟/复位与实际模式，保留 TX、PHY 与 HD0 视频时序。 |

验收包含 `/dev/dri/card0`、V0 的 `IN_FORMATS`、vblank 计数与刷新率一致、两种布局的 DMA 导入及 Atomic Pageflip、VDP 活动地址和格式回读、无 underflow、VDH SMMU INTID 181 保持零。HDMI 画面以采集或现场观察单独判定。Main10 的低位面参与扫描和 HDMI 深色链路分别验收。报告各样本有效帧数、扫描提交/完成数、两种 UHD 样本的墙钟 FPS 与 CPU 口径、镜像 SHA256 和 U-Boot RAM 回读 CRC32。

## Phase 2 · V0 缩放、G0 叠加与 SDR 色彩

- V0 在既有 Tiled DMA-BUF 上使用 ZME1，允许整个帧缓冲缩放至 CRTC 内偶数对齐的矩形。目标尺寸各轴支持源尺寸的 0.5–8 倍。1:1 使用 Phase 1 的旁路与最近行色度扩展。放大轴启用亮度线性 FIR。色度保留最近邻，避免 Phase 1 在实验垂直色度 FIR 下出现的衰减。
- G0 以固定 zpos 1 注册 DRM Overlay Plane，V0 为 zpos 0。G0 支持线性 ARGB8888 与 XRGB8888、标准 `alpha` 与 `pixel blend mode`。G0 走固件已启用的 GP0 画布。在本次 UHD 时序下，目标矩形是 G0 帧缓冲的 2 倍，位置按两个输出像素对齐。两层共用 HD0 VT1，翻页事件在待更新寄存器清除且两层的活动地址匹配后交付。
- V0 的标准 `COLOR_ENCODING` 与 `COLOR_RANGE` 支持 BT.601 / BT.709 / BT.2020 和限幅 / 全幅。输出保持 HDMI 固件的 BT.709 限幅时序。V0 CSC 对非默认输入重编码 YCbCr，保留传递函数编码后的 R′G′B′ 分量。BT.709 限幅直通。PQ 色调映射、线性光 RGB 原色变换及 HDMI HDR 元数据属于后续工作。 [DRM KMS 平面属性](https://docs.kernel.org/gpu/drm-kms.html)

| 数据路径 | 原厂依据与寄存器 |
|---|---|
| V0 IFIR 与 ZME1 几何 | `xdp_drv_set_vid_ifir`（`c05e83f8`）设置 `0x2600[2:1]=3`，在 `0x2604–0x2610` 写入八个固定系数。 `vdp_drv_set_vid_zme1`（`c05e8540`）写入水平 Q20 比例 `0x22c0[23:0]`、垂直 Q12 比例 `0x22dc[15:0]`、相位与 `0x22e8` 输出尺寸。Y/C 共用输入尺寸比，水平相位均为 0，垂直相位 Y 为 0、C 为 −512。1080p→2160p 的比值字段分别为 `0x80000` 和 `0x0800`。 |
| ZME1 FIR | `vdp_drv_set_zme_coef`（`c05e8adc`）、`drv_vdp_tranlate_zoom_coef`（`c05e68ec`）、`vdp_vid_set_zme_coef_addr`（`c0614368`）与 `vdp_vid_set_para_upd`（`c0615434`）给出 DDR 加载链路。四表顺序为 H-Y、H-C、V-Y、V-C，槽间距 256 字节。每 128 bit 包含 12 个 10-bit 系数。驱动在 `0x0e04/14/0c/1c` 写入地址，向 `0x0f00` 写 `0xf` 请求更新。水平 Y FIR 为 `0x22c0[26]`，垂直 Y FIR 为 `0x22d8[24]`。采用 8/4/6/4 抽头表布局，系数使用线性权重。ZME1 以 `0x22d8[20:19]=1` 接收 4:2:0，以 `[22:21]=0` 输出 4:2:2，沿固件链路送 VP0。 |
| G0 与 CBM | 原厂 DT 的 `virtdev` 将 VDP IRQ 定为 SPI 90。G0 共享 VDP MMIO。 `vdp_fdr_gfx_set_ifmt`（`c05fbbd0`）指定 `0x8a40=0x68`。`0x8a50/54` 写入地址、`0x8a60` 写入 16 字节单位的 stride，`0x8b38` 为活动地址。 `vdp_gfx_set_layer_galpha`（`c05f1134`）对应 `0x5000[7:0]`。 `0x8a08[1:0]` 控制逐像素 Alpha 与 0–255 范围，bit 10 选择预乘。CBM 的 `0x6908=0x21` 将 G0 接入 GP0，HD0 的 `0x0208=0x21` 从底到顶选择 VP0、GP0。 |
| V0 CSC | `xdp_drv_set_vid_csc`（`c05e9674`）、`vdp_func_set_csc1_{coef,dc_coef}`（`c05ee840`、`c05ee6e8`）给出 Q12 矩阵、符号位和 DC 坐标。转换启用时 `0xb000[4:0]=0x13`，`0xb004–0xb024` 为九个系数，`0xb050–0xb064` 为输入输出 DC。限幅场景输入减去 Y=64、Cb/Cr=512，全幅场景 Y 的输入 DC 为 0。输出加回 64/512/512。系数写入后触发 `0x4004` 使 VP0 锁存。 |

指定的原厂 [ARM32 反编译报告](artifacts/vpu-reverse-factory/arm32-result.txt) 用于 VDEC/取数链路核对。上表的显示函数由同一原厂内核的 kallsyms 与二进制复核。显示路径以 [原厂 DT 摘要](logs/04-factory-dtb-summary.txt)、[HDMI 寄存器记录](HDMI-REVERSE.md) 和板端回读佐证。

原厂 `g_y2r_m33_*` 表是一组 YCbCr→RGB Q12 矩阵，输入 DC 表 `c106569c` 和 `c10656b8` 分别为 `(-64,-512,-512)` 与 `(0,-512,-512)`。当前驱动使用等效的 YCbCr→BT.709 限幅 YCbCr 矩阵，混合器接收 YCbCr。BT.601 限幅沿用原厂 `g_pq_gfx_csc_table_ycbcr601l_to_ycbcr709l` 的 Q12 换算值。

| 原厂输入编码 | 限幅表地址与九个系数 | 全幅表地址与九个系数 |
|---|---|---|
| BT.601 | `c10656e8` · 4784, 0, 6557, 4784, −1610, −3340, 4784, 8279, 0 | `c10656fc` · 4096, 0, 5743, 4096, −1410, −2925, 4096, 7251, 0 |
| BT.709 | `c10656c0` · 4784, 0, 7365, 4784, −876, −2190, 4784, 8678, 0 | `c10656d4` · 4096, 0, 6451, 4096, −768, −1918, 4096, 7601, 0 |
| BT.2020 | `c106566c` · 4784, 0, 6896, 4784, −770, −2672, 4784, 8799, 0 | `c10656a4` · 4096, 0, 6040, 4096, −674, −2341, 4096, 7706, 0 |

### Phase 2 后续 · VDEC 原生队列生命周期

- 在 Phase 1 配对原型基础上验证较深 DPB、重排序及多 slice。根据请求时间戳、flip/out-fence、DRM FB 引用和流关闭顺序安排缓冲回收；验证暂停、背压、丢帧、多个解码上下文及持续运行。涉及的 V0 寄存器继续按 Phase 1 索引核对。启用 VDP 私有 SMMU 时，单独管理 `0xe000`、`0xe2ec`、TLB 失效和错误恢复。
- 验收包括 8-bit、Main10 持续解码与 Atomic Pageflip、旧缓冲在停止被扫描和引用后回收、Streamoff 后无滞留引用或 DMA 故障，线性格式逐帧回归通过。根据实测固定 modifier 布局、地址/尺寸限制和对象生命周期，再规划公开 UAPI 编号。

## Phase 3 · 4K60 与播放器生态

- 接入独立 HD0 modeset、海思 HDMI bridge、EDID/HPD、CRG 门控/复位和 PHY。依据 `HDMI-REVERSE.md` 的 TX `0xf8ce0000`、AVI `0xf8ce1800`、DDC/AON `0xf8ce4000`、PHY `0xf8d30000` 补齐时序与链路。按 sink 能力处理 HDR metadata、输出位宽、色域矩阵与 YCbCr range。
- 为 FFmpeg V4L2 Request 构造带正确 modifier 和存储面数量的 `AVDRMFrameDescriptor`，验证 mpv DRM PRIME overlay。验收包括动态模式切换及热插拔、HDR 与 SDR 像素、UI 叠加、Main/Main10 原生缓冲的稳定 2160p60 播放、帧率与延迟分位数、CPU 与温度记录。

## RAM 验证边界

远程通过 `scripts/remote.ps1` 交叉编译。COM4 以 115200 8N1 由单进程持有，TFTP 网络变量仅在 U-Boot RAM 中设置。对 `0x01fdffc0` 的上传镜像做 `crc32` 回读，Linux 仅从内置 initramfs 启动并确认无块设备挂载。沿用已验证的 BL31 只读加载步骤。禁止写 U 盘 `/boot/boot.img`、刷写 eMMC、触碰 OTP 或执行 `saveenv`。

构建时将新版 VDEC/VDP 源码、DTS、共享头文件、测试程序、`0012` 至 `0014` 补丁和测试样本同步至远程 `inputs/`，并同步 `scripts/build_arm64.sh`。已应用旧版 `0012` 的源码树需将 `VIDEO_HI3798MV320_VDEC` 的 `select VIDEOBUF2_DMA_CONTIG` 与新版补丁对齐。构建产物目录为 `out/release-arm64-7.3.5-vdp/`。

```bash
cd /home/hedc/hg680kd-mainline
VDEC_TEST=1 VDEC_FIXTURES='h265ip1080 hevc10ip1080 hevc4k hevc10ip4k ip32' \
  bash scripts/build_arm64.sh "$PWD" "$PWD/inputs" 7.3.5-vdp
```

单帧的最后两个参数是帧数与保持时间（毫秒）。省略它们会播放整个 fixture。测试程序按最大同时存活的解码参考面和当前输出分配 CAPTURE，再预留最多四个显示缓冲。一个 `poll` 循环同时处理解码请求和非阻塞 Atomic 翻页。收到新帧翻页事件且旧帧最后一次 DPB 引用完成后，旧面才能重新入队。翻页 FPS 采用首尾事件间的平均间隔，`wall_fps` 另计第一次提交前至末次事件后的吞吐。CPU 使用这一墙钟区间的 `/proc/stat` 四核合计，`usr` 含 nice，`sys` 含硬件和软件中断。

```sh
vdec-request /dev/video0 /dev/media0 /etc/vdec-test/hevc10ip4k.txt \
  /etc/vdec-test/hevc10ip4k.265 --display /dev/dri/card0 1 9000
```

主机测试用 `scripts/vdp_ram_test.py --image <boot.img> --log <日志>`。加 `--capture-device 'USB2 Video'` 保存 HDMI 采集图。`--capture-reference` 将 160×90 RGB 平均绝对误差与 24/255 上限及交换 U/V 后的误差比较。`--payload` 可从主机 HTTP 向板上 `/tmp` 发送更长的 HEVC fixture，`--linear` 可逐帧核对已有 NV21/P010 参考。采集卡将 2160p HDMI 输入输出为 1080p MJPEG，可核对画面形状和颜色。HDMI 链路的逐像素 10-bit 验证需要深色采集设备。

Phase 2 的主机参数 `--scale` 让 V0 占满固件模式。 `--overlay argb` 在两个 G0 测试缓冲间轮换，`--independent` 额外提交一次只更新 G0 的非阻塞翻页。 `--overlay xrgb --alpha 32768` 验证全局 Alpha。缩放采集用 `--single h265ip1080 --single hevc10ip1080 --capture-reference artifacts/arm64-7.3.5-vdp`。 240 帧叠加压力测试用 `--overlay argb --bench-only --payload artifacts/arm64-7.3.5-vdp/phase1-async 4k60-8 4k60-10`。指定非默认色彩时，使用 `--encoding` 和 `--color-range`，其默认值为 BT.709 限幅。

## 2026-11-27 · Phase 1 板测结果

**DMA 导入、翻页、消隐中断和线性回归通过。HDMI 色度仍待修复，Phase 1 整体未验收。** 实机走 U-Boot 的 3840×2160p60 时序，HD0 总计 4400×2250。1920×1080 源居中 1:1 扫描。板上注册 `/dev/dri/card0`、`/dev/video0`、`/dev/media0`。正式运行中的 1489 次 VDP IRQ 均采到 `HD0_STATE[1]=1`，在单帧保持期间 INTID 122 每秒约增加 60。INTID 181 保持为零，累计 `unexpected_status=0`，内核日志没有 VDP underflow、超时或 SMMU 错误。本板次未实测 1080p60 HDMI 链路自身的固件交接。[正式串口日志](artifacts/arm64-7.3.5-vdp/phase1-board.txt) · [1080p 8-bit FB/IRQ 复核](artifacts/arm64-7.3.5-vdp/fb-recheck.txt)

| 源 | `0x8240` | Y / VU `0x8254/5c` | 低位 Y / VU `0x8284/8c` | 主 / 低位 pitch `0x8290/94` |
|---|---:|---|---|---|
| 1080p Main | `0x0000000a` | `0x67500000 / 0x67720000` | `0 / 0` | `0x08000800 / 0` |
| 1080p Main10 | `0x0000010a` | `0x67500000 / 0x67720000` | `0x67830000 / 0x678b8000` | `0x08000800 / 0x02000200` |
| 2160p Main | `0x0000000a` | `0x67500000 / 0x67cf8000` | `0 / 0` | `0x0f000f00 / 0` |
| 2160p Main10 | `0x0000010a` | `0x67500000 / 0x67cf8000` | `0x680f4000 / 0x682f2000` | `0x0f000f00 / 0x03c003c0` |

四组单帧均收到 Atomic flip event。`0x2004=0`、`0x2018=0xe00000ff`、`0x8338=0x67500000`，活动 Y 地址匹配提交值。`0x8220=3`、`0xe000=1`。1080p 与 UHD 的 3 帧短样本各完成 3 次翻页并重排队 2 次。4K 单向连续试验如下，fixture 由 `testsrc2`、libx265、32 帧 I/P 样本生成。两轮各用 3 个 CAPTURE 缓冲并重排队 31 次。解码器 `detile=0`、`sync=0`，每帧仍初始化输出和私有工作区域。[正式串口日志](artifacts/arm64-7.3.5-vdp/phase1-board.txt)

正式日志里的首个 1080p Main FB/IRQ 判定受样本名匹配错误影响。脚本已修正并在同一镜像上复核通过，HDMI 色度判定仍为失败。[复核日志](artifacts/arm64-7.3.5-vdp/fb-recheck.txt)

| 4K 输入 | 完成 | FPS | 四核 CPU | 每帧输出初始化 |
|---|---:|---:|---:|---:|
| HEVC Main | 32/32 | 20.90 | 4.89% | 7.93 ms |
| HEVC Main10 | 32/32 | 20.60 | 5.46% | 9.45 ms |

线性像素比对总计 14/14。H.264 NV21 32×32 为 2/2，HEVC NV21 1080p 和 2160p 分别为 3/3，HEVC P010 1080p 和 2160p 分别为 3/3。测试程序串行等待解码和翻页，FPS 仅用于这个原型路径的摸底。[逐帧 SHA256](artifacts/arm64-7.3.5-vdp/phase1-board.txt#L883)

HDMI 采集可辨认两种位深和两种输入分辨率的图像。彩条的位置和亮度结构匹配，色度偏绿，四张首帧比较的 RGB MAE 为 84.12–84.41/255。原始 YUV 对照中，测试红色色条的输入为 `Y=81, Cb=90, Cr=240`。经 V0 后采集值为 `82,10,115`，同一色值经 VP0 mute 直接输出时为 `82,74,254`。V0 的 FDR mute 重现衰减，ZME1 和 ZME2 独立工作时也可重现。上述结果将后续排查范围缩小到 V0 输入至 VP0 混合前的色度处理。Main10 的低位地址和 pitch 已编程并回读，HDMI 深色链路及低 2 位的独立像素校验仍待验证。[采集与参考对照](artifacts/arm64-7.3.5-vdp/phase1-hdmi-vs-reference.png) · [色度诊断](artifacts/arm64-7.3.5-vdp/csc-test.txt)

| 镜像校验 | 值 |
|---|---|
| `boot.img` 大小 | 17,369,160 bytes |
| SHA256 | `73eb3b95859d2a752c8f3a3d29f1f21d0305f009769df36b7f953438500995ac` |
| U-Boot RAM 回读 CRC32 | `b8f83792` |

镜像由 `hedc@ArchLinux-E5-Server` 使用 GCC 14.2.0 构建。COM4 以 115200 8N1 独占，通过 TFTP 加载至 `0x01fdffc0` 并核验 CRC32，Linux 根位于 RAM。`mmcblk0/ro=1`，没有块设备挂载。未写 U 盘 `/boot/boot.img`、eMMC、OTP 或 U-Boot 持久环境。[构建日志](artifacts/arm64-7.3.5-vdp/build.txt) · [镜像](artifacts/arm64-7.3.5-vdp/boot.img) · [TFTP 日志](artifacts/arm64-7.3.5-vdp/phase1-board.tftp.txt)

## 2026-11-27 · Phase 1 色彩验收与异步 4K60 复测

**四种原生输入的 HDMI 色彩通过验收，两组 4K 样本均持续翻页 60.00 FPS。** 本轮保持固件交接的 3840×2160p60 HDMI 时序，1920×1080 内容在输出内居中 1:1 扫描。采集卡输出 1080p 8-bit MJPEG，验收范围包含 Main10 输入经 V0 上屏后的颜色。HDMI 深色链路和逐像素的 10-bit HDMI 输出精度仍待深色采集。[实机串口](logs/487-vdp-phase1-async.txt) · [采集与参考对照](logs/487-vdp-phase1-async-hdmi.png)

色度衰减位于 V0 到 VP0 之前。FDR mute 注入同一 YCbCr 仍可复现，V0 layer mute 与 VP0 mute 输出一致。对照原厂 `xdp_drv_set_vid_ifir`（`c05e83f8`）恢复 `0x2600[2:1]=3`，把 `-7,12,-17,25,-38,58,-105,328` 装入 `0x2604–0x2610`。实验垂直 FIR 的衰减随系数装载和窗口位置变化，同一 1080p 图在零纵向偏移时可恢复。1:1 路径使用 `0x22d8=0xc0080001`，保留 4:2:0→4:2:2 最近行扩展，关闭 ZME FIR。VP0 接收 YCbCr，按固件链路旁路 V0 CSC/HDR。主 VU 地址与 pitch 保持原生值，Main10 回读 `0x8240=0x10a` 以及非零的低位 Y/VU 地址和 pitch。[V0、VP0 和 FIR 诊断记录](logs/488-vdp-color-and-vt1-diagnostics.txt)

| 输入 | HDMI 对软解 RGB MAE | HDMI 对 U/V 互换参考 RGB MAE |
|---|---:|---:|
| 1080p Main | 3.29/255 | 116.12/255 |
| 1080p Main10 | 2.88/255 | 116.15/255 |
| 4K Main | 3.13/255 | 116.05/255 |
| 4K Main10 | 2.65/255 | 116.06/255 |

同一组 240 帧 I/P 样本使用两个参考帧。每种位深分配 7 个 CAPTURE，DMA-BUF 预导入，一次保持一个待完成翻页。原消隐末端的 VT1 每次跨过两个 vblank，异步但未调整阈值时两组均为 30.00 FPS。把 VT1 移到后肩前部后，活动 Y 地址匹配时可提早确认锁存，并给下一次提交留下完整的帧间隔。原生 HEVC 的全宽整 tile 路径仅清空对齐区及私有 guard/PMV，4K 每帧清零均值降至 Main 1.31 ms、Main10 1.34 ms。其余 CAPTURE 布局保留逐帧初始化。[初次异步实测和 VT1 阈值记录](logs/488-vdp-color-and-vt1-diagnostics.txt) · [最终板测](logs/487-vdp-phase1-async.txt#L910)

| 4K 输入 | 翻页完成 | 翻页 FPS / 墙钟 FPS | CPU usr / sys / idle | 丢帧 / 错失 vblank | VDP IRQ 122 增量 | HD0 underflow | SMMU IRQ 181 增量 |
|---|---:|---:|---:|---:|---:|---:|---:|
| HEVC Main | 240/240 | 60.00 / 59.99 | 0.00% / 5.18% / 94.82% | 0 / 0 | 241 | 0 | 0 |
| HEVC Main10 | 240/240 | 60.00 / 59.78 | 0.19% / 5.42% / 94.40% | 0 / 0 | 241 | 0 | 0 |

线性 NV21/P010 逐帧 SHA256 共 14/14，与 FFmpeg 参考一致。NV21 的 H.264 32×32 为 2/2，HEVC 1080p 和 4K 各 3/3。P010 1080p 和 4K 各 3/3。[逐帧 SHA256](logs/487-vdp-phase1-async.txt#L1027)

另用诊断程序映射回读两组 4K 原生 CAPTURE，将可见像素解排布后逐帧与 FFmpeg 比较 CRC32。Main 和 Main10 各匹配 12/12，均覆盖七个 CAPTURE 索引和回收后的再次写入。Main10 CRC 包含 Y、U、V 的实际低 2 位。该诊断读回单独执行。[原生像素 CRC32](logs/487-vdp-phase1-async-native-crc.txt)

| 最终纯 RAM 镜像校验 | 值 |
|---|---|
| `boot.img` 大小 | 17,369,160 bytes |
| SHA256 | `a58f4f756c1a400165cf8aea8c6e615d21e34d3140ec466ba7c24f80bf2f7a50` |
| U-Boot RAM 回读 CRC32 | `5c7b89d6` |

镜像在 `hedc@ArchLinux-E5-Server` 的 `/home/hedc/hg680kd-mainline` 构建，经 COM4 单进程独占 TFTP 纯 RAM 启动。板端根文件系统为 `rootfs`，无块设备挂载，`mmcblk0/ro=1`。BL31 走原有只读加载流程。本机镜像位于 `artifacts/arm64-7.3.5-vdp/phase1-async/final/boot.img`。[构建](logs/487-vdp-phase1-async-build.txt) · [串口校验](logs/487-vdp-phase1-async.txt#L145) · [TFTP](logs/487-vdp-phase1-async.tftp.txt)

## 2026-11-27 · Phase 2 板测验收

V0 的 720p/1080p ZME1 全屏缩放、4K 原生直扫、G0 叠加及 SDR CSC 通过本轮验收。1080p 使用 2×、720p 使用 3× 放大。采集画面的彩条、斜边和细棋盘与软解参考对应，未观察到偏绿、花屏或明显边缘伪影。MAE 使用 160×90 RGB，采集设备的输出为 1080p 8-bit MJPEG。[HDMI 对照图](logs/489-vdp-phase2-hdmi.png) · [缩放与直扫串口](artifacts/arm64-7.3.5-vdp/phase2/validated/scale/board.txt)

| 输入 | V0 输出 | HDMI 对 FFmpeg RGB MAE | HDMI 对 U/V 互换参考 RGB MAE |
|---|---|---:|---:|
| 720p Main | UHD 全屏 3× | 8.62/255 | 121.68/255 |
| 1080p Main | UHD 全屏 2× | 3.02/255 | 116.29/255 |
| 1080p Main10 | UHD 全屏 2× | 2.66/255 | 116.26/255 |
| 4K Main | 1:1 | 3.13/255 | 116.05/255 |
| 4K Main10 | 1:1 | 2.65/255 | 116.06/255 |

G0 的 ARGB Coverage 在 1080p 放大与 4K Main/Main10 直扫时出图，G0-only 非阻塞提交均收到独立翻页事件。4K Main 的透明区采集像素保持 `(0,255,0)`，128/255 Alpha 的黑底覆盖同一绿色输入后为 `(0,145,0)`。XRGB8888 也通过独立翻页，0x8000 全局 Alpha 下同一取样点为 `(0,145,0)`。GP0 使用固件的 1920×1080 画布。实测将 GP0 改为 3840×2160 单位比会导致 HD0 underflow 和花屏。[叠加像素取样](artifacts/arm64-7.3.5-vdp/phase2/validated/overlay/pixels.txt) · [G0 事件和活动地址](artifacts/arm64-7.3.5-vdp/phase2/validated/overlay/board.txt)

| V0 4K60 输入与 G0 ARGB 叠加 | 翻页 | 翻页 FPS / 墙钟 FPS | 四核 usr / sys / idle | 丢帧 / 错失 vblank | IRQ 122 增量 | VDP underflow | SMMU IRQ 181 增量 |
|---|---:|---:|---:|---:|---:|---:|---:|
| HEVC Main | 240/240 | 60.00 / 59.96 | 0.06% / 4.99% / 94.94% | 0 / 0 | 241 | 0 | 0 |
| HEVC Main10 | 240/240 | 60.00 / 59.97 | 0.06% / 5.06% / 94.88% | 0 / 0 | 241 | 0 | 0 |

压力测试每帧同时切换 V0 的原生 Tiled DMA-BUF 和 G0 的线性缓冲。每组使用 7 个 CAPTURE、回收 239 次，`detile=0`、`sync=0`。两组 4K 关闭 G0 后再次翻页 240/240，均为 60.00 FPS，墙钟分别为 59.88 和 59.80 FPS。线性 NV21/P010 与 FFmpeg 的逐帧 SHA256 匹配 14/14。[叠加压力串口](artifacts/arm64-7.3.5-vdp/phase2/validated/overlay/bench.txt) · [4K 单层与线性回归](artifacts/arm64-7.3.5-vdp/phase2/validated/regression/board.txt)

| 输入色彩属性 | V0 CSC 回读 | HDMI 对软解 RGB MAE |
|---|---|---:|
| BT.709 限幅 | `0xb000=0x02`，旁路 | 4K Main 3.13/255，Main10 2.65/255 |
| BT.601 限幅 | `0xb000=0x13`，Y 输入 DC −64 | 9.92/255 |
| BT.709 全幅 | `0xb000=0x13`，Y 输入 DC 0 | 8.52/255 |
| BT.2020 限幅 Main10 SDR | `0xb000=0x13`，Y 输入 DC −64 | 9.34/255 |

三个转换样本均从 720p 放大。色彩属性显式选择，BT.709 全幅后切至 BT.2020 限幅，再以默认属性恢复 BT.709 限幅。恢复后 `0xb000=0x02`，4K Main MAE 仍为 3.13/255。[色彩和回归摘要](logs/489-vdp-phase2-acceptance.txt)

实际 HDMI 交接模式为 3840×2160p60，AVI 为 YCbCr 4:2:0、BT.709 限幅，`0xf8ce1800[1:0]=0`，`0xf8ce1a08[1]=0`，HDR 通用数据包头 `0xf8ce18a8=0`。原厂驱动有 `vdp_drv_set_hdr_cfg`、`vdp_drv_set_hdr_vcoef` 和 `drv_hdmi_drm_infoframe_send` 链路。带 BT.2020、PQ、Mastering Display 和 MaxCLL 的 HDR10 样本通过 Main10 解码、V0 原生导入及翻页。当前固件交接保持 8-bit SDR，验收仅覆盖样本输入和扫描。HDR InfoFrame、深色传输、PQ 色调映射和线性光宽色域变换仍待独立链路支持，Connector 未注册 `HDR_OUTPUT_METADATA`。[固件数据包回读](artifacts/arm64-7.3.5-vdp/phase2/validated/firmware.txt) · [HDR10 输入板测](artifacts/arm64-7.3.5-vdp/phase2/validated/hdr10/board.txt)

| 纯 RAM 镜像校验 | 值 |
|---|---|
| 大小 | 17,369,160 bytes |
| SHA256 | `54e4179c37da9c8b072046333f279b662eeec24d8a0ffdb3512ecc8b7018edf8` |
| U-Boot RAM `0x01fdffc0` 回读 CRC32 | `b13a17d7` |

使用远程 GCC 14.2.0 构建。COM4 单进程独占，以 115200 8N1 经 TFTP 启动并回读 CRC32。板端根为 `rootfs`，无块设备挂载，`mmcblk0/ro=1`。BL31 按原有流程只读加载，未写入 U 盘、eMMC、OTP 或持久环境。[镜像](artifacts/arm64-7.3.5-vdp/phase2/validated/boot.img) · [构建](artifacts/arm64-7.3.5-vdp/phase2/validated/build.txt) · [TFTP](artifacts/arm64-7.3.5-vdp/phase2/validated/scale/board.tftp.txt)

## Phase 3 · MP4 / MKV 播放器

mpv 的 `--vo=drm` 使用软件像素格式和 swscale。`--drm-drmprime-video-plane` 属于 `drmprime-overlay` 硬件互操作路径。本次使用 `vdp-player`，它通过 libavformat 读取 MP4 / MKV，通过 libavcodec 的 `v4l2request` 解码，并使用 libdrm 提交 Atomic KMS。板端沿用 Phase 2 的 3840×2160p60 固件时序和 SDR BT.709 限幅 HDMI 输出。 [mpv 的 DRM VO 源码](https://github.com/mpv-player/mpv/blob/master/video/out/vo_drm.c) · [mpv DRM plane 选项](https://mpv.io/manual/stable/#drm-drmprime-video-plane-primary-overlay-n)

FFmpeg 补丁在 V4L2 Request 设备上接收 `mv320_tiled=1` 选项。按位深显式协商 `H8TV` 或 `H0TV`，并按 CAPTURE 的 pitch 和对齐高度构造单对象 `AVDRMFrameDescriptor`。8-bit 使用 `DRM_FORMAT_NV21` 和两个面。Main10 使用私有 `H0TV` 和四个面。两者的 modifier 均为 `0xfe00000000000320`。Main10 4K 的四面 pitch 为 `3840/3840/960/960`，offset 为 `0/8355840/12533760/14622720`。普通 FFmpeg V4L2 Request 会话沿用线性 CAPTURE 格式。

播放器使用五帧有界解码队列和容器 PTS 驱动显示。V0 以源显示比例适配输出，16:9 的 1080p 用 V0 放大到全屏，4K 为 1:1。`--osd` 用两个线性 ARGB8888 缓冲绘制 G0 进度条，画布由硬件从 1920×1080 放大到 3840×2160。`COLOR_ENCODING` 和 `COLOR_RANGE` 取自解码后的 `AVFrame`，随 V0 帧提交。当前帧的引用保留到替换帧的 KMS 锁存事件，FFmpeg 另持有 DPB 引用。Seek 由解码线程在丢弃待显示队列后执行 `av_seek_frame()` 和 `avcodec_flush_buffers()`，以目标前的关键帧恢复解码。EOS 排空重排序队列，退出时先关闭扫描，再释放最后一帧和解码上下文。

远程构建使用 `hedc@ArchLinux-E5-Server` 上的 GCC 14.2.0、musl 和仓库固定的 FFmpeg 修订 `b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935`。同步两个 FFmpeg patch 到远程 `scripts/`，播放器 C 文件与共享 ABI 头到 `src/`，即可通过 `scripts/remote.ps1` 执行以下构建和样本准备。三个主验收样本取自 Blender Foundation《Big Buck Bunny》4K60 版的第 45 至 63 秒，均有 1,080 个视频包和有效时间戳。Main10 由 8-bit SDR 母片重新编码，以 10-bit 格式解码。1080p H.264 使用 `direct=auto`。 [Blender 片源](https://peach.blender.org/download/)

~~~bash
cd /home/hedc/hg680kd-mainline
bash scripts/build_vdec_ffmpeg.sh "$PWD" player
source_video="$PWD/out/vdec-ffmpeg/bbb_sunflower_2160p_60fps_normal.mp4"
bash scripts/make_vdp_player_clips.sh "$source_video" "$PWD/out/vdp-player/serve"
bash scripts/make_vdp_player_clips.sh "$source_video" "$PWD/out/vdp-player/serve" color
~~~

将 `out/vdp-player/serve/` 下载到主机的 `artifacts/arm64-7.3.5-vdp/phase3/serve/`。串口脚本先 TFTP 启动 Phase 2 验收镜像并回读 CRC32，再从主机 HTTP 校验并加载播放器与片源到板上 `/tmp`。`--color-switch` 额外测试单个 MKV 内的 SDR 矩阵及限幅切换。

~~~powershell
$phase3 = 'artifacts/arm64-7.3.5-vdp/phase3'
New-Item -ItemType Directory -Force "$phase3/final" | Out-Null
python -u scripts/vdp_player_ram.py `
  --image artifacts/arm64-7.3.5-vdp/phase2/validated/boot.img `
  --payload "$phase3/serve" --log "$phase3/final/board.txt" `
  --capture-device 'USB2 Video' --color-switch
~~~

板端直接运行的命令如下。`--seek AT:TO` 以流内时间触发跳转，允许连续指定。`--start` 和 `--stop-at` 用于从关键帧附近开始并提前关闭播放器。

~~~sh
cd /tmp
./ld-musl-aarch64.so.1 --library-path . ./vdp-player main10-2160p60.mkv
./ld-musl-aarch64.so.1 --library-path . ./vdp-player main-2160p60.mp4
./ld-musl-aarch64.so.1 --library-path . ./vdp-player --osd h264-1080p60.mp4
./ld-musl-aarch64.so.1 --library-path . ./vdp-player --seek 2:9 --seek 11:3 --seek 5:16 main10-2160p60.mkv
./ld-musl-aarch64.so.1 --library-path . ./vdp-player --start 2 --stop-at 4 main10-2160p60.mkv
~~~

## 2026-11-27 · Phase 3 HG680KD 板测

三段 18 秒容器文件均播放至 EOS。每组 PXP INTID 169 增加 1,080，VDP INTID 122 增加 1,081。VDP underflow、fetch underflow 和 VDEC SMMU INTID 181 均为 0。VDEC 原生路径报告 `detile=0`、`sync=0`，并由 `DRM_PRIME` 导入单个 DMA-BUF 直接出图。 [板端验收记录](logs/490-vdp-phase3-acceptance.txt)

| 容器与输入 | 解码 / 显示 | 显示 / 墙钟 FPS | 丢帧 / 错失 vblank | 四核 usr / sys / idle | 进程峰值 RSS |
|---|---:|---:|---:|---:|---:|
| `main10-2160p60.mkv` · HEVC Main10 | 1080 / 1080 | 60.000 / 59.521 | 0 / 0 | 2.59% / 4.81% / 92.60% | 89,180 KiB |
| `main-2160p60.mp4` · HEVC Main | 1080 / 1080 | 60.000 / 59.576 | 0 / 0 | 2.73% / 5.38% / 91.89% | 89,288 KiB |
| `h264-1080p60.mp4` · H.264 High + G0 进度条 | 1080 / 1080 | 60.000 / 59.766 | 0 / 0 | 1.55% / 6.54% / 91.91% | 22,784 KiB |

显示 FPS 由首末 KMS 翻页时间戳计算。墙钟 FPS 和四核 CPU 覆盖预缓冲至播放结束。Main10 进程峰值 RSS 为 87.1 MiB，系统 `MemAvailable` 在播放前、播放中最低采样和结束后分别为 1,939,164、1,685,520、1,957,112 KiB。同期 `CmaFree` 为 388,640、202,452、388,564 KiB。由于原生解码面由内核 CMA 分配，进程 RSS 和 CMA 分别列示。

1080p 输入按 16:9 由 V0 全屏放大，ZME1 读回水平 `0x80000`、垂直 `0x0800` 的比例字段。G0 以 ARGB8888 1080p 全画布覆盖 4K，并以 Coverage 逐像素混合。18 秒内完成 100 次 G0 翻页。HDMI 录像是 1920×1080、30 fps、301 帧，画面抽查未见撕裂，进度条在采集第 2 秒和第 7 秒分别填充约 116 和 544 像素。 [HDMI 运动帧](logs/490-vdp-phase3-hdmi.png)

同一首帧的 HDMI 图与软件解码参考相比，160×90 RGB 平均绝对误差为 7.74/255。交换参考的 U/V 后，误差为 27.26/255。G0 区域外的像素在开关叠加时一致。半透明黑底采样位置的 RGB 中位数从 (55, 108, 30) 变为 (26, 51, 13)。

Main10 同一进程按 2→9、11→3、5→16 秒跳转，再正常播放至 EOS。三次跳转共显示 480 帧，播放器丢帧为 0，另丢弃关键帧预滚 120 帧。PXP IRQ 增加 624，包含跳转前已排队的解码帧。退出后 `CmaFree` 比开始少 40 KiB。独立起播、停止连续三轮，每轮显示 120 帧，CMA 空闲值每轮恢复到开始值。跳转及重启过程的 VDP underflow 和 SMMU IRQ 181 均为 0。

`color-switch.mkv` 在同一播放会话中依次切换 BT.709 限幅、BT.601 全幅、BT.2020 限幅和 BT.709 限幅。回读 `0xb000` 依次为 `0x02 / 0x13 / 0x13 / 0x02`。两次启用 CSC 时，输入 Y DC 分别为 `0` 和 `0x7c0`。480 帧完整显示，四个解码上下文各完成 120 帧，PXP IRQ 合计增加 480，VDP IRQ 增加 481，underflow 和 SMMU IRQ 181 为 0。

使用同一构建的 FFmpeg CLI 回归默认线性 CAPTURE。H.264 NV21 和 HEVC Main10 P010 各 240 帧，逐帧哈希与软件解码参考一致。每组 PXP IRQ 增加 240，SMMU IRQ 181 为 0。 [线性 CAPTURE 回归](logs/490-vdp-phase3-acceptance.txt#L267)

| 镜像及 RAM 校验 | 值 |
|---|---|
| Phase 2 `boot.img` 大小 | 17,369,160 bytes |
| SHA256 | `54e4179c37da9c8b072046333f279b662eeec24d8a0ffdb3512ecc8b7018edf8` |
| U-Boot `0x01fdffc0` RAM 回读 CRC32 | `b13a17d7` |

COM4 以 115200 8N1 单进程独占。系统从 TFTP 上传的镜像在 RAM 启动，播放器及素材经校验后加载到 `/tmp`。根文件系统为 `rootfs`，块设备挂载数为 0，`mmcblk0/ro=1`。U-Boot 环境变量仅在 RAM 设置。 完整构建、串口、TFTP 和 HDMI 录像保存在本地的 `artifacts/arm64-7.3.5-vdp/phase3/`。 [播放器、运行库和片源 SHA256](logs/490-vdp-phase3-acceptance.txt#L12)
