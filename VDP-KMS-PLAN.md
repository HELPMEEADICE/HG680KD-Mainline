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
| `0x04/08/0c`、`0xc228/0xc2a0` | IRQ 状态、W1C 清除、mask。HD0 VT1 使用 bit 0，Linux 把阈值移到最后一条活动线，在消隐状态 `0xc2a0[1]` 置位后记账。采样原始 IRQ 状态以识别取数异常。 |
| `0x2000/04/18`、`0x2080–8c`、`0x8338` | V0 enable、更新触发、活动控制、1:1 视频区域和活动 Y 地址。更新位清除且活动控制、活动地址已匹配时交付翻页事件。 |
| `0x22c0/0x22d8–e8`、`0xe0c/0xe1c/0xf00` | ZME1 的 1:1 亮度设置与 4:2:0 → 4:2:2 色度路径。使用 DMA 参数区装载垂直滤波系数。 |
| `0x4000/04/10`、`0x4080–94`、`0x4100/08`、`0x208` | V0 经 VP0 进入固件已配置的 HD0 mixer。按 `xdp_drv_set_vp0_reso`、`xdp_drv_set_vid_cbm` 和 `xdp_drv_set_reg_up` 设置 VP0 全屏范围、背景、alpha、V0 优先级并触发锁存。 |
| `0x8210/20/40` | VU 顺序位为 0。主 Y/VU 2D 取数 bypass 为 1。`data_type=2, data_fmt=1, data_width=0/1, dcmp_type=0`，复核逐行显示模式。 |
| `0x8244/48/4c` | 源分辨率、裁剪起点、取数尺寸。以有效像素尺寸编程，pitch 和偏移使用分配尺寸。 |
| `0x8250/54/58/5c`、`0x8280/84/88/8c` | 主 Y/VU、低 2 位 Y/VU 各自的高低地址。8-bit 清低位面配置，32 位 bypass 下清地址高字。 |
| `0x8290/94` | 主/低位面 pitch，低半字 Y、高半字 VU。 |
| `0xe000[0]`、`0xe2ec` | 核验 VDP 全局 bypass 与非安全 TTBR。仅 V0 平面在本阶段使用 DMA 直通。 |
| CRG `0xd8`、`0x34c`、HDMI AVI | 读取并记录固件时钟/复位与实际模式，保留 TX、PHY 与 HD0 视频时序。 |

验收包含 `/dev/dri/card0`、V0 的 `IN_FORMATS`、vblank 计数与刷新率一致、两种布局的 DMA 导入及 Atomic Pageflip、VDP 活动地址和格式回读、无 underflow、VDH SMMU INTID 181 保持零。HDMI 画面以采集或现场观察单独判定。Main10 的低位面参与扫描和 HDMI 深色链路分别验收。报告各样本有效帧数、扫描提交/完成数、两种 UHD 样本的墙钟 FPS 与 CPU 口径、镜像 SHA256 和 U-Boot RAM 回读 CRC32。

## Phase 2 · VDEC 原生 Tiled 路径与队列生命周期

- 在 Phase 1 配对原型基础上验证较深 DPB、重排序及多 slice。根据请求时间戳、flip/out-fence、DRM FB 引用和流关闭顺序安排缓冲回收；验证暂停、背压、丢帧、多个解码上下文及持续运行。涉及的 V0 寄存器继续按 Phase 1 索引核对。启用 VDP 私有 SMMU 时，单独管理 `0xe000`、`0xe2ec`、TLB 失效和错误恢复。
- 验收包括 8-bit、Main10 持续解码与 Atomic Pageflip、旧缓冲在停止被扫描和引用后回收、Streamoff 后无滞留引用或 DMA 故障，线性格式逐帧回归通过。根据实测固定 modifier 布局、地址/尺寸限制和对象生命周期，再规划公开 UAPI 编号。

## Phase 3 · 4K60 与播放器生态

- 接入独立 HD0 modeset、海思 HDMI bridge、EDID/HPD、CRG 门控/复位和 PHY。依据 `HDMI-REVERSE.md` 的 TX `0xf8ce0000`、AVI `0xf8ce1800`、DDC/AON `0xf8ce4000`、PHY `0xf8d30000` 补齐时序与链路。按 sink 能力处理 HDR metadata、输出位宽、色域矩阵与 YCbCr range。G0 的 `0x5000/04` 和 FDR `0x8a00` 用作独立 UI plane。
- 为 FFmpeg V4L2 Request 构造带正确 modifier 和存储面数量的 `AVDRMFrameDescriptor`，验证 mpv DRM PRIME overlay。验收包括动态模式切换及热插拔、HDR 与 SDR 像素、UI 叠加、Main/Main10 原生缓冲的稳定 2160p60 播放、帧率与延迟分位数、CPU 与温度记录。

## RAM 验证边界

远程通过 `scripts/remote.ps1` 交叉编译。COM4 以 115200 8N1 由单进程持有，TFTP 网络变量仅在 U-Boot RAM 中设置。对 `0x01fdffc0` 的上传镜像做 `crc32` 回读，Linux 仅从内置 initramfs 启动并确认无块设备挂载。沿用已验证的 BL31 只读加载步骤。禁止写 U 盘 `/boot/boot.img`、刷写 eMMC、触碰 OTP 或执行 `saveenv`。

构建时将新版 VDEC/VDP 源码、DTS、共享头文件、测试程序、`0012` 至 `0014` 补丁和测试样本同步至远程 `inputs/`，并同步 `scripts/build_arm64.sh`。已应用旧版 `0012` 的源码树需将 `VIDEO_HI3798MV320_VDEC` 的 `select VIDEOBUF2_DMA_CONTIG` 与新版补丁对齐。构建产物目录为 `out/release-arm64-7.3.5-vdp/`。

```bash
cd /home/hedc/hg680kd-mainline
VDEC_TEST=1 VDEC_FIXTURES='h265ip1080 hevc10ip1080 hevc4k hevc10ip4k ip32' \
  bash scripts/build_arm64.sh "$PWD" "$PWD/inputs" 7.3.5-vdp
```

单帧的最后两个参数是帧数与保持时间（毫秒）。省略它们会播放整个 fixture，逐次等待 flip event，并按 manifest 中的最后引用帧回收 CAPTURE 缓冲。FPS 从第一次提交解码到最后一次翻页完成计时，CPU 为 `/proc/stat` 四核合计忙碌占比。

```sh
vdec-request /dev/video0 /dev/media0 /etc/vdec-test/hevc10ip4k.txt \
  /etc/vdec-test/hevc10ip4k.265 --display /dev/dri/card0 1 9000
```

主机测试用 `scripts/vdp_ram_test.py --image <boot.img> --log <日志>`。加 `--capture-device 'USB2 Video'` 保存 HDMI 采集图。`--capture-reference` 可与软解首帧 PNG 比较，按 160×90 RGB 平均绝对误差 24/255 判定这组彩条的画面检查。`--payload` 可从主机 HTTP 向板上 `/tmp` 发送更长的 HEVC fixture，`--linear` 可逐帧核对已有 NV21/P010 参考。采集卡将 2160p HDMI 输入输出为 1080p MJPEG，可核对画面形状和颜色。Main10 的 HDMI 深色链路、逐像素低 2 位还需额外采集能力或硬件校验。

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
