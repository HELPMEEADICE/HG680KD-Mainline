# HG680KD 连续硬解板测

## 默认 Direct 与 Cacheable Tiled 输出

原镜像运行默认 Direct 的 H.264 片源时，FFmpeg 报告输出 240 帧，驱动只完成 15 帧，PXP INTID 169 增量为 19。首次失败的 PXP 状态为 `0x00060000`，SMMU 状态为 `0x0`，UP_MSG 的 `[0]`、`[2]`、`[32]` 分别为 `0x94210000`、`0x00000d08`、`0x94230000`。该任务的 `slice[7]=0x0001810a`，bit 15 已置位，bit 14 被清零。诊断日志保留了对应的 PMV 地址、参考面和顶底 POC，以及 L0/L1 表。[旧镜像复现](logs/469-vdec-direct-baseline-perf.txt) · [失败任务](logs/471-vdec-direct-messages.txt)

原厂 `dps_h264_make_dps_slice` 把 SPS 的 `direct_8x8_inference_flag` 写入 slice 描述的 `+0x29`，`hal_h264_write_slice_msg` 在 `c07a9274–c07a92c4` 将它写入 `slice[7]` bit 14，随后才分流 I/P/B。只在 B slice 恢复 bit 14 时 PXP 完成 240 个任务，默认多参考样本匹配 40 帧。所有 I/P/B slice 都按 SPS 写入 bit 14 后，默认多参考及单参考对照样本各匹配 240/240 帧。PMV 在参考图像解码时建立，后续 B slice 读取 L1[0] 的 ColMV。bit 15 由 slice 的空间 Direct 标志决定。`pub[23]` 与 `slice[8]` 分别指向当前面和 L1[0] 的 PMV，`pic[10..25]`、`pic[27..58]` 按同一 DPB 槽存放参考地址和 POC。L0 逆表的两个 5-bit 项都填入首个匹配的列表索引，避免重复引用使索引按位叠加。[原厂消息生成](artifacts/vpu-reverse-factory/arm32-result.txt) · [板端字段对照](logs/475-vdec-direct-ab.txt)

对照组使用同一 Direct 修复和旧 coherent 解排布。优化组为内部参考面的 8-bit、2-bit 平面与 PMV 使用 `dma_alloc_noncoherent(DMA_BIDIRECTIONAL)`，在提交、完成读回及再次用作 DPB 引用时同步 DMA 所有权。工作区使用 coherent DMA。NV21 按 tile 行组拷贝 64 B，Main10 每次用 NEON 合成 16 个 P010 采样，边缘及不可用 SIMD 的场景用 64-bit 批量拼包。两组使用同一 FFmpeg、同一组 240 帧 MP4 和 `schedutil`，每段各跑三轮。H.264 样本含 125 个 P 帧、113 个空间 Direct B 帧和 2 个 I 帧，37 个 B 帧被标记为参考。CPU 数据为四核合计，DMA 同步单独计时。[Linux DMA API](https://docs.kernel.org/core-api/dma-api.html) · [编码文件 SHA256](logs/449-vdec-ffmpeg-clips.txt)

| 片源 | FPS 前 → 后 | 整板 usr / sys / idle (%) 前 → 后 | 解排布 ms/帧 前 → 后 | 优化组 DMA 同步 ms/帧 |
|---|---:|---:|---:|---:|
| 1080p H.264 默认 Direct | 19.57 → 53.61 | 0.64 / 23.65 / 75.71 → 2.49 / 26.07 / 71.44 | 44.94 → 6.60 | 4.01 |
| 1080p HEVC Main | 20.01 → 63.73 | 0.67 / 24.67 / 74.66 → 2.96 / 27.67 / 69.38 | 45.41 → 6.16 | 3.35 |
| 4K HEVC Main | 7.02 → 23.62 | 0.31 / 23.80 / 75.89 → 1.21 / 24.27 / 74.52 | 128.92 → 17.46 | 8.24 |
| 4K HEVC Main10 | 3.15 → 15.23 | 0.14 / 24.75 / 75.12 → 0.50 / 23.72 / 75.78 | 303.44 → 44.95 | 5.85 |

优化镜像的四段样本均逐帧匹配软解，每段 240/240。两组的每轮基准都有 240 个硬件完成帧和 240 次 PXP 中断，SMMU 计数在两次正式 RAM 启动内均为零。4K Main10 的 VDH+PXP 均值为优化前 9.537 ms/帧、优化后 9.535 ms/帧。候选字段试验中，原厂之外的参考场类型取值曾触发 SMMU，正式验证从重新启动后的零计数开始。[优化前原始计数](logs/480-vdec-coherent-perf.txt) · [优化后原始计数](logs/482-vdec-cached-perf.txt) · [24 轮 CSV](logs/483-vdec-cache-measurements.csv) · [逐帧哈希校验](logs/484-vdec-cached-recheck.txt)

两组都在 `hedc@ArchLinux-E5-Server:/home/hedc/hg680kd-mainline` 用 GCC 14.2.0 编译 Linux 7.3.5。COM4 115200 8N1 独占，经 TFTP 装入 `0x01fdffc0`，U-Boot CRC32 与主机一致。板上仅使用 RAM 根文件系统，`mmcblk0/ro=1`。对照镜像 CRC32 为 `51e9c84b`，优化镜像 CRC32 为 `1184cae4`。优化镜像 SHA256 为 `85c49679a487e2ed26a1b7eb4000a1bf317f0359347cb3f671112c78d5e8b6c3`。[对照镜像启动](logs/479-vdec-coherent-check.txt) · [优化镜像构建](logs/478-vdec-cached-build.txt) · [优化镜像启动](logs/481-vdec-cached-check.txt) · [boot.img](artifacts/arm64-7.3.5-vdec-direct/boot.img)

补充回归覆盖 520×280 Main10 的行尾和底部不完整 tile，以及 128×128 H.264 的双 slice 空间 Direct 和时间 Direct。三段均为 240/240 像素匹配。优化镜像本次启动最终累积 5760 次 PXP 中断、0 次 SMMU 中断，内核日志中无 PXP 解码或 SMMU 错误。[Main10 边缘回归](logs/485-vdec-cached-regression.txt) · [H.264 回归与最终中断](logs/486-vdec-h264-regression.txt)

```powershell
$work = 'artifacts/arm64-7.3.5-vdec-direct'
python scripts/vdec_ffmpeg_ram.py --image "$work/boot.img" --payload "$work/payload" --mode check --log logs/vdec-cached-pixels.txt
python scripts/vdec_ffmpeg_ram.py --mode perf --runs 3 --log logs/vdec-cached-perf.txt
```

## 首轮 FFmpeg 测试

2026-11-22 在 HG680KD（四核 Cortex-A53、2 GiB RAM）上运行 Linux `7.3.5-hg680kd-arm64-vdec`。计时内核由 `hedc@ArchLinux-E5-Server:/home/hedc/hg680kd-mainline` 交叉编译，再经 COM4 115200 8N1 独占控制，用 TFTP 装入 `0x01fdffc0`。U-Boot 回读 CRC32 `d66e640e`，镜像 SHA256 为 `94a2fe7ab35b9502f17edb9cdba345a0ab1b4894032600ae9197f6494b0c85e5`。U-Boot 只读加载原厂 BL31，Linux 从 RAM 根文件系统运行。板上 `mmcblk0/ro=1`，没有挂载块设备，应用、码流和测试文件均在 `/tmp`。未修改 U 盘 `/boot/boot.img`，未刷写 eMMC、操作 OTP 或执行 `saveenv`。[远程内核构建](logs/453-vdec-perf-kernel.txt) · [TFTP 与 CRC32 复核](logs/454-vdec-ffmpeg-correctness.txt) · [板端最终状态](logs/465-vdec-ffmpeg-board-end.txt) · 本地镜像 `artifacts/arm64-7.3.5-vdec-perf/boot.img`

## 片源与硬解确认

从 4K60 版《Big Buck Bunny, Sunflower version》的 `00:07:25` 起截取 240 帧。四种输出都是 60 fps，两个闭合 GOP，含运动画面与 I/P/B 帧。源片为 8-bit，Main10 样本经 10-bit 编码，软解首帧的 12,441,600 个采样中有 4,776,110 个采样的低两位非零。[Blender 原片下载](https://bbb3d.renderfarming.net/download.html) · [编码元数据和 SHA256](logs/449-vdec-ffmpeg-clips.txt)

原 RAM 镜像没有 FFmpeg。本次交叉构建 Kwiboo 的 `v4l2-request-n8.1` 分支 `b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935`，补充线性 NV21 的 DRM 格式映射，以独立 musl loader、libdrm 和 libudev 在板端加载。`-hwaccels` 列出 `v4l2request`。串口日志记录了 `/dev/media0`、`/dev/video0`、Media Request 设备选择和 CAPTURE 格式。驱动兼容了 FFmpeg 以 slice payload 长度填写 HEVC `bit_size` 的方式，同时保留既有测试程序使用的整段长度。[FFmpeg 分支](https://github.com/Kwiboo/FFmpeg/tree/b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935) · [交叉构建脚本](scripts/build_vdec_ffmpeg.sh) · [构建日志](logs/451-vdec-ffmpeg-build.txt) · [板端像素验证](logs/455-vdec-ffmpeg-correctness.txt)

| 片源 | 码率 Mb/s | I / P / B | 硬解与软解像素哈希 | PXP / SMMU 增量 |
|---|---:|---:|---:|---:|
| 1080p H.264 High 8-bit | 10.41 | 2 / 80 / 158 | 240/240 | 240 / 0 |
| 1080p HEVC Main 8-bit | 9.36 | 2 / 61 / 177 | 240/240 | 240 / 0 |
| 4K HEVC Main 8-bit | 22.63 | 2 / 61 / 177 | 240/240 | 240 / 0 |
| 4K HEVC Main10 | 22.30 | 2 / 61 / 177 | 240/240 | 240 / 0 |

H.264 基准用 x264 Medium、CRF 20、`keyint=120:min-keyint=120:scenecut=0:bframes=2:b-adapt=0:b-pyramid=none:ref=1:weightp=0:weightb=0:direct=none`。早期 x264 默认 Direct 的多参考样本出现 PXP `DECODE_ERROR`，随后因缺少已解码的参考帧返回 `-ENOENT`。关闭 B-pyramid 和加权预测、限制为单参考之后，仍有 137 个 B 帧失败，80 个 P 帧、2 个 I 帧和 21 个 B 帧与软解一致。最后再关闭 Direct 运动预测，240 帧均正确。H.264 性能数据适用于这组编码设置，默认 B 帧路径的兼容性问题仍可复现。[H.264 失败请求](logs/457-vdec-ffmpeg-h264-diag.txt) · [首轮 B 帧哈希](logs/458-vdec-ffmpeg-h264-frames.txt) · [最终编码](logs/460-vdec-ffmpeg-h264-no-direct-source.txt) · [最终硬解哈希](logs/462-vdec-ffmpeg-h264-candidates.txt)

三段 HEVC 用 x265 Fast、CRF 20、`pools=6:frame-threads=3:keyint=120:min-keyint=120:scenecut=0:bframes=3:ref=4:open-gop=0`。4K 为 3840×2160。1080p 从同一原片用 Lanczos 缩放。8-bit 与 Main10 编码分别使用 `yuv420p` 和 `yuv420p10le`，封装为 MP4。四个样本和软解参考随 FFmpeg 一起保存在板测目录。本地测试文件位于 `artifacts/arm64-7.3.5-vdec-perf/payload`

## 帧率与分阶段耗时

每段连续运行三次。FPS 为三次各自平均 FPS 的均值，括号内为最小值和最大值。其余耗时列为三次的均值。

| 片源 | FPS（范围） | 总耗时 s | RBSP ms/帧 | VDH+PXP ms/帧 | 解排布 ms/帧 | 清零 ms/帧 | 纯硬件 FPS 上限 | 去解排布 FPS 估算 | PXP / SMMU 每次增量 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1080p H.264 High | 19.17（19.12–19.20） | 12.52 | 0.193 | 3.925 | 45.770 | 0.928 | 254.8 | 156.7 | 240 / 0 |
| 1080p HEVC Main | 20.15（19.83–20.44） | 11.91 | 0.177 | 2.491 | 45.121 | 0.959 | 401.5 | 221.3 | 240 / 0 |
| 4K HEVC Main | 7.04（7.02–7.05） | 34.11 | 0.245 | 9.512 | 128.612 | 2.410 | 105.1 | 74.1 | 240 / 0 |
| 4K HEVC Main10 | 3.17（3.14–3.18） | 75.82 | 0.246 | 9.539 | 301.709 | 2.907 | 104.8 | 70.4 | 240 / 0 |

RBSP 包含 NAL 头复制和剥离 `00 00 03` 的扫描写入，多 slice 以帧为单位累加。硬件耗时从 VDH 提交前计到 PXP 完成中断，排除了线程唤醒和后续复位。解排布包含 Y 和色度两平面，亮度主面块为 64 B×16 行，色度为 64 B×8 行。Main10 还读取 64 采样×2 行的低位块、拼接采样并交换 UV。清零包含 coherent DMA 参考面和 PMV 区的 `memset`。成功完成的帧才进入统计，每个流停止时输出一次汇总。[驱动计时点](src/hi3798mv320-vdec.c)

端到端 FPS 用 `240 / 板端墙钟秒数` 计算。墙钟在 FFmpeg 进程前后由 `/proc/uptime` 采集，含初始化、解码与资源释放。纯硬件估算为 `1000 / VDH+PXP ms`。去解排布估算用 `1000 / (每帧墙钟 ms − 解排布 ms)`，保留 RBSP、清零、队列等成本，假定其他成本和调度条件维持原样。两列估算均未运行实际 Tiled 直出。每次接受的基准要求 FFmpeg 帧数、驱动成功帧数和 PXP INTID 169 增量均为 240，并核对 SMMU INTID 181 的起止计数均为零。[H.264 三轮原始计数](logs/464-vdec-ffmpeg-h264-perf.txt) · [HEVC 九轮原始计数](logs/459-vdec-ffmpeg-hevc-perf.txt)

## CPU 占用

使用默认 `schedutil`，CPU policy 最大频率为 1,000 MHz。基准起止温度快照为 59–63 °C，频率快照多为 800 MHz，个别结束点为 400 MHz。整板占用以四核合计时间为分母，`usr=user+nice`，`sys=system+irq+softirq`，`idle` 为原始 idle 列。单核等效为整板活跃率乘以四，表示耗用的算力，线程可在核间迁移。`FFmpeg 单核等效` 来自 `-benchmark` 的 `utime+stime`，除以墙钟。内核解排布由驱动工作线程执行，计入整板 `sys`。

| 片源 | 整板 usr / sys / idle | CPU0 / CPU1 / CPU2 / CPU3 活跃率 | 整板单核等效 | FFmpeg 单核等效 |
|---|---:|---:|---:|---:|
| 1080p H.264 High | 0.59% / 23.60% / 75.81% | 24.54% / 24.50% / 24.64% / 23.14% | 96.76% | 5.78% |
| 1080p HEVC Main | 0.63% / 24.77% / 74.60% | 22.55% / 25.08% / 27.24% / 26.68% | 101.61% | 7.94% |
| 4K HEVC Main | 0.26% / 24.06% / 75.68% | 25.76% / 24.88% / 22.25% / 24.39% | 97.28% | 4.63% |
| 4K HEVC Main10 | 0.17% / 24.84% / 74.99% | 13.55% / 14.15% / 56.52% / 15.82% | 100.03% | 3.27% |

各轮的每核 usr、sys、idle，FFmpeg 的 utime、stime、rtime，以及温度和频率快照均可在明细中回查。[基准 CSV](logs/466-vdec-ffmpeg-measurements.csv)

## 瓶颈与优化优先级

1080p60 每帧只有 16.67 ms。H.264 和 HEVC 的解排布分别耗时 45.77 和 45.12 ms，占端到端 87.8% 和 90.9%。保留其余成本时，达到 60 fps 分别允许 10.28 和 12.15 ms 的解排布。

4K30 的预算为 33.33 ms/帧，4K60 的预算为 16.67 ms/帧。4K 8-bit 和 Main10 的解排布分别占墙钟 90.5% 和 95.5%，是两种目标帧率的主要瓶颈。仅压缩 RBSP 拷贝最多能省去约 0.25 ms/帧。保留其余成本时，达到 4K30 分别允许 19.83 ms 和 19.13 ms 的解排布，达到 4K60 只剩 3.17 ms 和 2.46 ms。Main10 的纯硬件等待与 8-bit 接近，当前降速主要来自 8+2-bit 读取、拼接和 P010 输出。

仅按有效像素计算，4K NV21 解排布每帧搬运约 24.88 MB，P010 每帧约 40.44 MB。达到 30/60 fps 时，仅这一阶段分别需要约 0.75/1.49 GB/s 和 1.21/2.43 GB/s 的有效读写吞吐。该计算没有计入硬件参考面访问、清零、缓存行为与额外拷贝。当前阶段耗时证明了 coherent DMA 读回和 CPU 转换路径的瓶颈，单凭这些计时无法确定 DDR 总线是否饱和。

1. 优先检查 coherent DMA 读端的映射属性和访问方式。测量顺序读与当前跨 tile 读取的吞吐，再用 tile 行组批量读、宽拷贝和写端顺序布局做 A/B 对照。更改 DMA 映射时需要核对 PXP 完成后的同步及硬件重用参考面的时序。
2. 为 P010 加整块 NEON 路径，批量解包低两位、扩展至 LE16 并交换色度 UV。以同一片源复核全帧哈希，再比较解排布耗时。重排单线程访存后，再验证按 tile 行分工是否能随核心数扩展。
3. 评估向 DRM/GPU 暴露可导入的原生布局及对应 modifier。4K60 需要用实际 Tiled 直出或流水并行验证端到端余量，同时保留 NV21/P010 供线性消费者使用。

## 复现命令

板端以 `/tmp` 为当前目录。性能基准不执行 `hwdownload`，直接把 DRM frame descriptor 送进 null muxer。驱动仍在 CAPTURE 完成前执行解排布。正确性运行才把 CAPTURE 下载至用户态并计算 `framemd5`。

```sh
/tmp/ld-musl-aarch64.so.1 --library-path /tmp /tmp/ffmpeg \
  -nostdin -hide_banner -nostats -benchmark -xerror -max_error_rate 0 \
  -hwaccel v4l2request -hwaccel_output_format drm_prime -i "$clip.mp4" \
  -map 0:v:0 -an -sn -dn -fps_mode passthrough -c:v wrapped_avframe \
  -progress perf.progress -f null -

/tmp/ld-musl-aarch64.so.1 --library-path /tmp /tmp/ffmpeg \
  -nostdin -hide_banner -loglevel verbose -xerror -max_error_rate 0 \
  -hwaccel v4l2request -hwaccel_output_format drm_prime -i "$clip.mp4" \
  -map 0:v:0 -an -sn -dn -vf "hwdownload,format=$pixels" \
  -fps_mode passthrough -threads:v 1 -c:v rawvideo -f framemd5 \
  -y "$clip.hw.framemd5"
```

`pixels` 取 `nv21` 或 `p010le`。软解使用主机 FFmpeg `n9.1`，按显示顺序对 240 个像素哈希逐项比较，剔除工具版本和时间戳字段差异。H.264 的 1920×1088 编码面由 FFmpeg 按 SPS 裁到 1920×1080 后比对。主机脚本负责串口、TFTP/CRC32、RAM 下载和判定，板端脚本负责命令、CPU/IRQ 快照和计时输出。[软件参考](logs/452-vdec-ffmpeg-software-reference.txt) · [主机脚本](scripts/vdec_ffmpeg_ram.py) · [板端脚本](scripts/vdec_ffmpeg_board.sh)

```powershell
$work = 'artifacts/arm64-7.3.5-vdec-perf'
$clips = 'h264-1080p-no-direct', 'hevc-1080p', 'hevc-2160p', 'hevc10-2160p'
python scripts/vdec_ffmpeg_ram.py --image "$work/boot.img" --payload "$work/payload" --mode check --log logs/vdec-pixel-recheck.txt @clips
python scripts/vdec_ffmpeg_ram.py --mode perf --runs 3 --log logs/vdec-perf-rerun.txt @clips
```
