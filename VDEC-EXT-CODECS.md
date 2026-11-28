# Hi3798MV320 VDH MPEG-1 和 MPEG-2

2026-11-28 · 烽火 HG680KD · Linux 7.3.6

## 格式选择

海思的 MV320 规格列出 MPEG-1、MPEG-2、MPEG-4 Part 2、AVS+ 和 AVS2。同板原厂内核的 `s_dec_hal_ops` 从 `0xc176bbf0` 起按 40 B 一项，MPEG-2 项在 `0xc176bc90`，首字是 `hal_mpeg2_write_msg`（`0xc07d82b4`），末字是 VFMW 软件协议号 4。MPEG-1 在同一路径用 `dps_mpeg2_make_dps_pic` 选择 MPEG-1 语法。MPEG-4 和 AVS2 有独立的 HAL 消息和辅助区，MPEG-4 还包含 MDMA 作业路径。现有 MV320 规格、原厂媒体配置和 VDH HAL 表均缺少 VP9 项。基于协议和驱动复用，选择 MPEG-2 和 MPEG-1。 [海思 MV320 产品规格](https://www.hisilicon.com/cn/products/smart-media/STB/Hi3798MV320) · [原厂符号](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt#L24861)

| OUTPUT | V4L2 接口 | 当前实现范围 |
| --- | --- | --- |
| `MG2S` / `V4L2_PIX_FMT_MPEG2_SLICE` | 每图一个 Media Request，携带标准 `MPEG2_SEQUENCE` 和 `MPEG2_PICTURE`。可提供 `MPEG2_QUANTISATION`。 | Progressive 8-bit 4:2:0，I/P/B，最大 1920×1080。CAPTURE 支持 NV21 和 8-bit MV320 tiled。 |
| `MPG1` / `V4L2_PIX_FMT_MPEG1` | 普通 M2M 队列。每个 OUTPUT 缓冲携带一个完整 picture 及其序列/GOP 头，驱动解析头部并持有前一参考图。支持 STOP、LAST、EOS 和 START。 | Progressive 8-bit 4:2:0，I/P，固定协商尺寸，最大 1920×1080。CAPTURE 为 NV21。 |

MPEG-2 参考由用户态通过 CAPTURE 时间戳指定，I/P/B 分别使用零、一、两个参考。用户态负责持有引用缓冲与恢复显示顺序。MPEG-1 可跨同尺寸 GOP 接续解码，当前覆盖一图一包的 I/P 流。 [Linux V4L2 MPEG-2 Stateless 控件](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-codec-stateless.html) · [V4L2 压缩格式](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-compressed.html)

## 原厂消息和 Linux DMA 布局

厂商地址来自同板 ARM32 镜像。表内 `pub`、`pic`、`core`、`slice` 的索引以 32-bit 字为单位，地址字段为 16 B 单位，像素步幅和偏移为字节。 [原厂内核分析方法](VPU-REVERSE.md#10-linux-735-v4l2-stateless-hevc-main-8-bit) · [Linux 实现](src/hi3798mv320-vdec.c)

| 区域 | 写入和地址 |
| --- | --- |
| Public DN，工作区 `+0x000` | `hal_mpeg2_write_pub_msg`（`0xc07d79c4`）给出 `pub[2] = 0x41400000 | (macroblocks - 1)`，`pub[3]` 低字节为 **VDH 硬件协议 3**，常规未压缩作业字为 `0x0003c003`。`pub[4..6]` 指向 picture DN、picture UP、码流，`pub[31]` 是减一的宏块几何。 |
| Core DN，`+0x100` | `core[28]` 指到 `+0xe00`，`core[32]` 指到首条 slice UP，`core[36]` 为 slice 描述符数。`core+0x500/0x504` 为前向/后向参考 Y 地址，`+0x540/0x544` 指向相应图像区末端，`+0x5cc` 起携带几何、步幅和色度偏移，`+0x614` 指向输出图像区末端。对应 `dec_hal_write_core_msg`（`0xc077b3e0`）和 `hal_mpeg2_write_msg`。 |
| Picture DN，`+0x800` | `pic[0]` 含宏块宽高和 MPEG-1 语法选择 bit 25。`pic[1..3]` 打包 picture type、frame structure、full-pel、f_code、DCT、量化和扫描标志。`pic[4..7]` 给出后向参考、前向参考、当前图和 PMV。`pic[9]` 为连续数据位数，`pic[63]` 指向首条 slice DN。 |
| 量化与参考 | `hal_mpeg2_write_pic_msg`（`0xc07d7aa0`）将帧内、非帧内量化矩阵写入 `pic[16..31]` 和 `pic[32..47]`。V4L2 的 zigzag 顺序转为按列、行的奇偶组打包。两种格式均用 VLC。MPEG-1 的参考 tiled 面与当前图面的同规格私有 DMA 区在完成时交换，CAPTURE 可立即由用户态归还。 |
| Slice DN，`+0xe00` | `hal_mpeg2_write_slice_list`（`0xc07d7df8`）确定每条 32 B。`slice[0]` 低 24 位是位数，高 7 位是段内位偏移，`slice[1]` 是相对于 `pub[6]` 的 16 B 单位偏移，`slice[7]` 链至下一条，最后一条清零。Linux 连续缓冲用第一段。逐 slice 生成描述符，首段数据从 `00 00 01` 后的垂直位置字节开始。 |
| UP | Picture UP `+0x32e00`，slice UP `+0x32e80`。原厂 `dec_hal_read_slc_msg` 以 16 B 为一条回报。提交前清空到 `+0x33c00`。首轮将整图压成一个 slice 描述符时，板上出现 PXP `0x60000`、SMMU `0`。同一输入逐条描述后通过。 |
| Scratch 和码流 | 共用 scratch 从 `+0x52000` 起保留 `0x11000` B。`pub[22/24/30]` 引用其中 `+0x4500/0x8100/0xbd00`。码流在 `+0x63000`，尾部清零 256 B。每个流关闭后释放工作区、量化消息和 MPEG-1 参考面。 |
| 像素面 | VDH 写 8-bit tiled VU，步幅为 `ALIGN(width, 256)`，Y 区为步幅乘 `ALIGN(height, 64)`，VU 紧随。NV21 用共用解排布输出可见尺寸，MPEG-2 tiled 可走既有 `videobuf2-dma-contig` 并导出 DMA-BUF。PMV 独立页对齐，在 tiled 面之后。私有 SMMU 动态页表及 PXP IRQ 沿用同一驱动。 |

## 测试复现

样本是 FFmpeg 生成的 1920×1080、30 fps、120 帧动态 `testsrc2`。MPEG-1 用 `-c:v mpeg1video -bf 0 -g 30 -q:v 4`，MPEG-2 用 `-c:v mpeg2video -bf 2 -g 30 -q:v 4`。原始码流和请求放在 `artifacts/arm64-7.3.6-vdec-ext/`。测试脚本用 FFprobe 的包位置将严格软件解码的 NV21 参考排列成作业顺序，逐帧比较三个平面。 [请求及参考生成器](scripts/vdec_mpeg_fixture.py) · [V4L2 测试工具](src/vdec-mpeg-test.c)

```powershell
$out = 'artifacts/arm64-7.3.6-vdec-ext'
python scripts/vdec_mpeg_fixture.py "$out/1080p.m1v" "$out/mpeg1-1080p"
python scripts/vdec_mpeg_fixture.py "$out/1080p.m2v" "$out/mpeg2-1080p"
python scripts/vdec_ext_ram.py $out
```

完整回归另需本目录的 `h264-2160p30-pad.mp4`、`h264-2160p60.mp4` 和 `venc-regression.nv12`，以及原验收目录的 Lima、JPGD、播放器和 VENC 工具。4K30 样本把 640×360 动态内容用 `pad=3840:2160:1600:896` 放入 4K 画面，用 libx264 High@5.1、`-preset veryfast -crf 24`、四 slice、120 帧、无 B 帧。4K60 压力样本用全幅 `testsrc2=size=3840x2160:rate=60`、libx264 High@5.2、`-preset ultrafast -crf 24`、120 帧、单参考、无 B 帧。VENC 输入用本节 1080p30 源输出 12 帧 NV12。

构建使用远端 `/home/hedc/hg680kd-mainline/out/venc-ffmpeg` 的已验收源码、配置与 RAM DTB。用 `scripts/remote.ps1` 把驱动、`pack_arm64.py` 和构建脚本放在 `out/arm64-7.3.6-vdec-ext/input/`，调用脚本时传远端根目录、空的输出目录和产物目录。测试工具用远端 `out/musl-arm64/bin/musl-gcc -idirafter out/headers-arm64/usr/include -static -O2 -Wall -Wextra -Werror` 编译并一并下载。 [构建脚本](scripts/build_vdec_ext_736.sh) · [COM4 验收脚本](scripts/vdec_ext_ram.py)

验收脚本独占 COM4（115200 8N1），核对原厂 BL31 副本和 `boot.img`，U-Boot 各自经 TFTP 读入 RAM 并做 CRC32 回读。根文件系统、测试输入和输出均在 RAM，验收脚本核对挂载表和块设备。COM4 流程只使用 RAM 环境变量、TFTP、CRC 和启动指令。

## 实机结果

远端 GCC 14.2.0 从空目录 `out/arm64-7.3.6-vdec-ext/clean.5Vii02bZ` 完整构建，编译日志零警告。内核版本为 `7.3.6-hg680kd-arm64-vdec-ext`，VDEC、JPGD、VENC、VDP、Lima 和 AIAO 内建。内核配置只修改了 LOCALVERSION，DTB 与已验收基线逐字节相同。镜像为 19,826,760 B，SHA256 为 `1d6be9d318ae3fd04526b7ce519ae1bf9a3359f4e63fed86e6ebed64b295810a`。U-Boot 在 `0x01fdffc0` 的整镜像回读 CRC32 为 `399138e9`，BL31 回读为 `fa236592`。 [构建与 COM4 回读记录](logs/538-vdec-ext-acceptance.txt)。镜像、完整构建日志与校验清单位于本地 `artifacts/arm64-7.3.6-vdec-ext/`。

| 1920×1080 NV21 | 帧数 | FPS | 平均 QBUF 至 CAPTURE DQBUF | CPU0 / 1 / 2 / 3 | 全批 Y / U / V PSNR | 最低逐帧 Y / U / V PSNR |
| --- | ---: | ---: | ---: | --- | --- | --- |
| MPEG-1 I/P | 120 | 122.237 | 8.050 ms | 2.04% / 56.12% / 1.01% / 0.00% | 70.22 / 66.65 / 67.03 dB | 68.15 / 63.69 / 64.12 dB |
| MPEG-2 I/P/B | 120 | 117.138 | 8.271 ms | 0.00% / 29.41% / 32.35% / 0.00% | 72.50 / 69.38 / 69.92 dB | 70.58 / 67.08 / 67.64 dB |

性能轮不写 YUV，画质轮各写出 373,248,000 B。两轮各自每格式正好增加 120 次 VDH PXP INTID 169。MPEG-2 另跑 120 帧 tiled CAPTURE，`VIDIOC_EXPBUF` 成功，177.317 FPS、5.381 ms/帧，PXP +120。各轮退出后 DMA-BUF 都为 0 个、0 B，CMA `reserved/used` 都回到 3193 页。全局 `CmaFree` 样点在 380344 至 380444 KiB 间波动。画质结果按逐帧 PSNR 验收，两种格式的 YUV 批 SHA256 也随报告附存。SMMU INTID 181 的累计计数自启动起保持为 0。 [逐帧数据](logs/538-vdec-ext-result.json) · [性能和资源串口摘录](logs/538-vdec-ext-acceptance.txt)

MPEG-1 样本含 4 I、116 P 帧，MPEG-2 样本含 5 I、36 P、79 B 帧。补测枚举了 `MPG1` 和 `MG2S`，核对两种 OUTPUT 队列的 Media Request 能力，并检查 MPEG-1 STOP 后的空 LAST、EOS、START。 [补测记录](logs/538-vdec-ext-h264-ab.json)

| 基线路径 | 实机结果 |
| --- | --- |
| HEVC Main10 4K60 + VDP | 1080 帧解码、1080 帧 tiled 直扫，60.000 FPS，零丢帧、零漏 vblank。PXP +1080，VDP +1090。HDMI 模式为 1920×1080@60，输入为 3840×2160。 |
| H.264 1080p60 + VDP | 1080 帧全解码、全上屏，60.000 FPS，零丢帧、零漏 vblank。 |
| H.264 4K30 + VDP | 全幅样本在完整媒体负载后的首轮上屏 63/120 帧。新镜像新启后复测同一码流上屏 120/120 帧、30.000 FPS，与原镜像结果相同。四 slice 带边框样本也上屏 120/120 帧。HDMI 60 Hz 下，播放器的 119 次计数对应每两次刷新翻页一次。 |
| H.264 4K60 + VDP | 全幅压力样本的两版镜像都解码 120/120 帧。新镜像上屏 13 帧、丢 107 帧，原镜像上屏 16 帧、丢 104 帧。VDH 平均硬件耗时分别为 16.55 和 16.50 ms。四 slice 带边框的 4K60 样本在两版镜像也出现丢帧。海思给出的 H.264 4K 规格为 30 fps。 |
| JPGD | `/dev/video1` 完成 1080p JPEG，INTID 129 +1。 |
| VENC | `/dev/video2` 完成 1080p 12 帧 H.264，1 I + 11 P，INTID 133 +12，FFmpeg 严格软解通过。 |
| 双 PP Lima | 单独 PP0 和 PP1 各通过 16 次像素比对，双 PP 同时通过 32 次。Mesa GLES 再通过 60 帧。 |
| AIAO | `/dev/snd/pcmC0D0p` 播放 864,000 帧 48 kHz PCM，INTID 116 +1800，XRUN 为 0。 |

H.264 4K60 的零丢帧上屏条件仍未达到。COM4 已用最终镜像重新纯 RAM 启动，最后抽查 SMMU INTID 181 和 DMA-BUF 均为 0。全程未修改 U 盘、eMMC 或 OTP，未执行 `saveenv`。 [媒体回归和 H.264 A/B 串口摘录](logs/538-vdec-ext-acceptance.txt) · [H.264 A/B 数据](logs/538-vdec-ext-h264-ab.json)
