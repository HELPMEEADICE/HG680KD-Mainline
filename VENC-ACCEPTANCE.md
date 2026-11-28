# HG680KD Linux 7.3.6 VENC 纯 RAM 实机验收

2026-11-28，烽火 HG680KD。验收镜像为 `7.3.6-hg680kd-arm64-venc`，以已通过单帧 VENC 探针和双 PP Lima 回归的 Linux 7.3.6 为基线。使用 AArch64 GCC 14.2.0 从空输出目录构建，内核、DTB 和静态 `venc-test` 的构建日志均为零警告。 [构建记录](logs/535-venc-7.3.6-acceptance.txt#L3)

镜像、测试工具、原始日志及四路码流输出到本地 `artifacts/arm64-7.3.6-venc/`。码流文件名为验收数据中的 `case` 加 `.h264`。 [验收数据与 SHA256](logs/535-venc-7.3.6-result.json)

## 镜像与设备

| TFTP 装载项 | RAM 地址 | 字节数 | SHA256 | U-Boot RAM 回读 CRC32 |
| --- | --- | ---: | --- | --- |
| `boot.img` | `0x01fdffc0` | 19,826,760 | `5ae17d5936db6652de5c02a915e049bd65d872c2a450f4bd84081e131af03986` | `9d7d8e4b` |
| 原厂 BL31 RAM 副本 | `0x08020000` | 262,144 | `5f32da6063d8c49d0c2cc9fc7a1667f0e2b75931ce4d73af842f3efc51a97b7b` | `fa236592` |

COM4 以 115200 8N1 由单进程独占。U-Boot 两次 TFTP 装载后回读 CRC32，匹配后执行 `bootm 1fdffc0`。Linux 根文件系统为 BusyBox initramfs。eMMC 节点禁用，内核未启用模块、USB 大容量存储、UAS 或 MTD。验收时 `/proc/partitions` 无块设备。U 盘 `/boot/boot.img` 写入、eMMC 写入、OTP 操作和 `saveenv` 执行次数均为 0。 [串口记录](logs/535-venc-7.3.6-acceptance.txt#L9)

`hi3798mv320-venc` 内建并注册为 `/dev/video2`。`VIDIOC_QUERYCAP.driver` 使用适合该字段长度的 `hi3798-venc`。VDEC 和 JPGD 分别绑定 `/dev/video0` 和 `/dev/video1`。`/dev/media0`、`/dev/dri/card0`、`/dev/dri/renderD128` 和 `/dev/snd/pcmC0D0p` 均为有效字符设备。已移除临时 `hi3798-venc-probe.patch`，正式镜像中没有探针的 debugfs 接口。

## 编码与双解码

每组用同一次 STREAMON 编码 60 帧 FFmpeg `testsrc2` 动态图案。OUTPUT 为单缓冲半平面 NV12 或 NV21，步幅 64 B 对齐，硬件源面高度 16 行对齐。CAPTURE 每缓冲交付一个 Annex-B access unit。所有帧均为 Constrained Baseline Level 4.2、CAVLC、固定 QP、单 Slice IDR，SPS/PPS 随每帧输出。测试工具逐帧校验 `bytesused`、KEYFRAME、时间戳和序号，并检查 STOP 返回空 LAST 缓冲。

FFmpeg（`N-120279-g9015d595a1-20250716`）用 `-xerror -err_detect explode` 解码并检查可见尺寸、帧数、NAL 顺序、IDR 和实际 Slice QP。PSNR 按原始输入的可见 Y/U/V 样本计算。板端请求解码跑完每组全部帧，每批 12 帧，输出的 NV21 与软件解码的对应批次 SHA256 一致。1080p 比对覆盖完整 1088 行编码面，PSNR 按 SPS 裁剪后的 1080 行计算。吞吐包含测试工具在 RAM 内读取输入和写出码流的开销，单帧时间从 OUTPUT QBUF 至两队列 DQBUF。 [逐批 SHA256 与原始指标](logs/535-venc-7.3.6-result.json)

| 码流 | 帧数 | FPS | 平均 / 最慢单帧（ms） | CPU0 / 1 / 2 / 3（%） | Y / U / V PSNR（dB） | VENC / VDEC IRQ 增量 |
| --- | ---: | ---: | ---: | --- | --- | --- |
| 720p NV12 QP 24 | 60 | 36.406 | 25.205 / 27.168 | 17.58 / 30.91 / 21.82 / 15.15 | 50.921 / 48.755 / 49.068 | +60 / +60 |
| 720p NV21 QP 32 | 60 | 45.328 | 20.442 / 24.627 | 0.00 / 76.52 / 3.03 / 0.00 | 42.888 / 42.214 / 42.314 | +60 / +60 |
| 1080p NV12 QP 32 | 60 | 19.299 | 47.845 / 57.992 | 12.54 / 25.40 / 30.87 / 12.86 | 43.377 / 42.824 / 43.008 | +60 / +60 |
| 1080p NV21 QP 24 | 60 | 19.184 | 47.307 / 56.857 | 6.37 / 23.64 / 33.23 / 18.85 | 50.900 / 49.671 / 50.000 | +60 / +60 |

## 资源与回归

驱动对私有 SMMU 使用全局旁路，TTBR 保持 0。硬件只访问驱动自有的 DMA 工作区。首段容量和基址分别写入 `+0x27c` 和 `+0x2bc`，硬件输出长度取 `+0x3844[28:0] - +0x3848[6:0]`，加上软件生成的参数集后回填 CAPTURE `bytesused`。

每个已提交作业先屏蔽 IRQ，再请求总线排空和复位，检查 `+0x3010` 回到 0，随后恢复 CRG。编码核心、AXI 和 SMMU 由一个组合时钟按原厂顺序控制，两个独占复位由 CRG reset provider 管理。若排空或复位失败，驱动保留其 DMA 工作区和时钟引用，隔离设备直至重启。本轮用正常作业验证排空，未注入总线故障触发隔离分支。

240 次 SPI 101（GIC INTID 133）与 240 次成功排空一一对应。每组 `STREAMOFF` 并关闭句柄后，DMA-BUF 为 0 对象、0 字节，CMA 区的分配位图计数均回到 3,193 页。CRG `+0x08c/+0x32c` 每组均回读为 `0x00000003/0x00000001`。VENC 私有 SMMU fault 和 GIC INTID 181 的累计计数全程均为 0。

| 配置 | `CmaFree` 编码前 → 关闭后（KiB） |
| --- | ---: |
| 720p NV12 QP 24 | 380,444 → 380,216 |
| 720p NV21 QP 32 | 380,284 → 380,216 |
| 1080p NV12 QP 32 | 380,284 → 380,164 |
| 1080p NV21 QP 24 | 380,384 → 380,164 |

全量验收中，四组编码和硬解环回全部关闭后，CMA 分配位图仍为 3,193 页，`CmaFree` 为 380,376 KiB，启动时为 380,444 KiB。基础媒体回归和 Mesa 完成后分别占用 3,209 和 3,217 页，最终 `CmaFree` 为 380,240 KiB。用同一镜像重新 TFTP 启动并只运行媒体回归，VENC IRQ 始终为 0，复现了相同的 3,193 → 3,209 → 3,217 页变化。 [对照计数](logs/535-venc-7.3.6-result.json)

| 回归项 | 结果 |
| --- | --- |
| 双 PP Lima | PP0、PP1 各 16 个任务，交替模式 32 个任务。每任务 4,096 像素全部匹配，INTID 126 分别 +32、+32、+64。 |
| Mesa Lima GLES | Mesa 25.2.7 从 `/dev/dri/renderD128` 使用 Mali450。每轮 60 帧 640×480 像素读回均通过。共享 INTID 126 实测 +179 或 +180，最终验收 +180。 |
| 1080p JPGD | `/dev/video1` 成功解码 1920×1080 JPEG，INTID 129 +1。 |
| 2160p60 VDEC / VDP | Main10 3840×2160 源解码和上屏均为 1,080 帧，零丢帧、零漏 vblank。当前 HDMI 模式为 1920×1080@60，VDP 将 2160p 源缩放输出。INTID 169 / 122 分别 +1,080 / +1,090。 |
| AIAO | 48 kHz PCM 播放 864,000 帧，零 XRUN，INTID 116 +1,800。 |

编码与媒体回归的内核日志无 VENC、VDEC、JPGD、VDP、Lima 或 AIAO 故障。启动日志另有基线中已记录的蓝牙 H5 短包和校验提示。 [串口记录](logs/535-venc-7.3.6-acceptance.txt)

## 单参考 IPPP 与 GOP 控制

`7.3.6-hg680kd-arm64-venc-ippp` 在上述基线上扩展 `/dev/video2`。`GOP_SIZE` 为 1–120，默认 30。I/P 固定 QP 各自支持 0–51，默认均为 28。`FORCE_KEY_FRAME` 在下一个硬件作业插入 IDR 并重启 GOP。CAPTURE 每帧输出一个 Annex-B access unit，IDR 带 SPS/PPS 和 KEYFRAME 标志，P 帧带 PFRAME 标志。

沿用两套重建面。GOP 内第 `f` 帧从 `f % 2` 槽取参考并向 `(f + 1) % 2` 槽重建，关联辅助面同步换槽，PME 信息用三槽轮换。P 帧编程写入原厂 HAL 的单参考搜索配置和本帧固定 QP。作业通过 IRQ、SMMU、总线排空和输出边界检查后才推进 GOP。发生作业错误、重启流或切换分辨率时，下一帧从 IDR 开始。 [原厂 HAL 与缓冲布局](VENC-H264-PLAN.md#22-recrefmvtmv-与码流)

GOP=1 保留 POC type 2 的首版 SPS。IPPP 使用 POC type 0，`frame_num=f`、`pic_order_cnt_lsb=2*f`，IDR 的 `idr_pic_id` 在 0、1 间交替。P Slice 采用默认单条 L0 参考，滑动窗口标记和非零 `nal_ref_idc`。 [ITU-T H.264](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.264-202408-I%21%21PDF-E&lang=e&type=items)

### 最终镜像与对比方法

2026-11-28 在 `hedc@ArchLinux-E5-Server` 用 AArch64 GCC 14.2.0 从空输出目录构建，内核和 DTB 构建日志零警告，静态 `venc-test` 通过 `-Werror`。文件输出到 `artifacts/arm64-7.3.6-venc-ippp/`。COM4 以 115200 8N1 独占。U-Boot 经 TFTP 将原厂 BL31 和镜像装入 RAM，回读 CRC32 后执行 `bootm 1fdffc0`。启动后根文件系统为 BusyBox initramfs，内核未启用模块，板端无持久块设备。未发起 U 盘 `/boot/boot.img` 或 eMMC 写入、OTP 操作及 `saveenv`。 [构建和串口记录](logs/536-venc-ippp-acceptance.txt)

| 装载项 | RAM 地址 | 字节数 | SHA256 | U-Boot RAM 回读 CRC32 |
| --- | --- | ---: | --- | --- |
| `boot.img` | `0x01fdffc0` | 19,826,760 | `e7936ac99cbc346c9489d202321b7407839c19a91f11f901058529a6e74b69dd` | `5c2d0ac8` |
| 原厂 BL31 RAM 副本 | `0x08020000` | 262,144 | `5f32da6063d8c49d0c2cc9fc7a1667f0e2b75931ce4d73af842f3efc51a97b7b` | `fa236592` |

主矩阵沿用默认 `schedutil` 调速策略。每对测试使用同一份 120 帧 FFmpeg `testsrc2` 运动序列、相同的 NV12/NV21 格式和 I/P QP。GOP=1 输出 120 个 IDR，GOP=30 输出 4 个完整的 30 帧 GOP。平均 FPS 包含 RAM 中输入读取与码流写出的开销，平均单帧时间从 OUTPUT QBUF 到两队列 DQBUF。码率按源序列 30 fps 换算。PSNR 按可见 Y/U/V 样本的合计平方误差计算。 [原始指标、输入和码流 SHA256](logs/536-venc-ippp-result.json)

| 格式 / QP | GOP | FPS | 平均单帧 ms | 总字节数 | Mbps | I / P 均值 B | Y / U / V PSNR dB | CPU0 / 1 / 2 / 3 % |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- | --- |
| 720p NV12 / 24 | 1 | 43.449 | 21.042 | 4,667,669 | 9.335 | 38,897 / — | 50.87 / 48.63 / 48.94 | 7.97 / 3.97 / 64.86 / 3.26 |
| 720p NV12 / 24 | 30 | 34.251 | 26.541 | 2,955,383 | 5.911 | 38,125 / 24,163 | 49.48 / 47.49 / 47.87 | 18.23 / 27.64 / 18.00 / 17.14 |
| 720p NV21 / 32 | 1 | 34.959 | 25.459 | 2,927,089 | 5.854 | 24,392 / — | 42.96 / 42.07 / 42.26 | 30.99 / 18.37 / 11.66 / 22.74 |
| 720p NV21 / 32 | 30 | 45.159 | 20.981 | 1,573,932 | 3.148 | 23,829 / 12,747 | 41.85 / 41.07 / 41.18 | 74.44 / 0.38 / 0.00 / 0.00 |
| 1080p NV12 / 32 | 1 | 19.361 | 47.213 | 5,503,843 | 11.008 | 45,865 / — | 43.49 / 42.70 / 43.00 | 19.84 / 15.02 / 25.16 / 20.97 |
| 1080p NV12 / 32 | 30 | 17.839 | 52.229 | 3,289,004 | 6.578 | 45,175 / 26,796 | 42.48 / 41.86 / 41.94 | 12.63 / 21.51 / 20.39 / 23.74 |
| 1080p NV21 / 24 | 1 | 19.138 | 47.362 | 8,924,439 | 17.849 | 74,370 / — | 50.87 / 49.54 / 49.88 | 10.69 / 29.78 / 11.00 / 29.71 |
| 1080p NV21 / 24 | 30 | 18.265 | 50.738 | 6,190,449 | 12.381 | 73,351 / 50,837 | 49.84 / 48.45 / 48.96 | 13.68 / 25.99 / 19.63 / 18.45 |

| 格式 / QP | GOP=1 字节数 ÷ GOP=30 字节数 | 码率降幅 | FPS 变化 |
| --- | ---: | ---: | ---: |
| 720p NV12 / 24 | 1.58× | 36.7% | -21.2% |
| 720p NV21 / 32 | 1.86× | 46.2% | +29.2% |
| 1080p NV12 / 32 | 1.67× | 40.2% | -7.9% |
| 1080p NV21 / 24 | 1.44× | 30.6% | -4.6% |

### 固定 CPU 调速策略的吞吐复测

同一纯 RAM 会话用 `performance` 复测每个配置，顺序为 GOP=1、30、30、1，每轮 120 帧。每轮前后 CPU 均为 1000 MHz，温度 60.0–62.0 °C，热保护保持启用。各码流 SHA256 与主矩阵对应输出一致，结束后恢复 `schedutil`。下表取两轮均值。 [单轮读数与恢复计数](logs/536-venc-ippp-result.json)

| 格式 / QP | GOP=1 FPS / ms | GOP=30 FPS / ms | FPS 变化 |
| --- | ---: | ---: | ---: |
| 720p NV12 / 24 | 45.421 / 20.245 | 44.699 / 20.991 | -1.6% |
| 720p NV21 / 32 | 46.458 / 20.044 | 45.403 / 20.872 | -2.3% |
| 1080p NV12 / 32 | 21.809 / 42.842 | 21.182 / 44.795 | -2.9% |
| 1080p NV21 / 24 | 21.331 / 43.240 | 20.831 / 45.026 | -2.3% |

### 码流、环回和控制项

FFmpeg `-xerror -err_detect explode` 软解全部 960 帧，ffprobe 与 `trace_headers` 核对 720p/1080p 尺寸、NAL 序列、Constrained Baseline/CAVLC、IDR/P 类型、`frame_num`、POC、参考标记和固定 QP。P 帧实测 `nal_ref_idc=3`，抽查 1080p 两路码流的四组 GOP 首尾图像，无可见参考花屏或漂移。四组 GOP=1 的前 60 帧分别与首版码流逐字节一致。对 IPPP，`/dev/video0` 每批从 IDR 开始解码完整 30 帧，每组四批 NV21 SHA256 均与软解一致，覆盖 1080p 的 1088 行编码面。GOP=1 也通过每组十批的逐批 SHA256 比对。各批解码输出与 IRQ 数一致，逐帧 Y/U/V PSNR 最低值收录于结果。 [逐批 SHA256 和逐平面 PSNR](logs/536-venc-ippp-result.json)

720p NV12 的独立控制项测试使用 I QP 24、P QP 32，在零基帧号 4 触发 `FORCE_KEY_FRAME`，得到 `IPPPIPPP`。同一文件描述符的重启测试在帧 4 执行 STOP/START，在帧 6 执行 STREAMOFF/STREAMON，得到 `IPPPIPIP`。两轮均通过 QP、CAPTURE 标志、序号、时间戳、严格软解与逐批硬解 SHA256 比对。 [控制项与重启串口记录](logs/536-venc-ippp-acceptance.txt)

### 资源回收与媒体回归

主矩阵 SPI 101（INTID 133）增量为 960，`+0x3010` 成功排空 960 次。每组关闭后 CMA 分配位图计数回到 3,193 页，DMA-BUF 为 0 对象、0 字节，CRG `+0x08c/+0x32c` 回到 `0x00000003/0x00000001`。私有 SMMU 保持全局旁路、TTBR=0，VENC SMMU fault 与 INTID 181 全程为 0。媒体回归后的 CMA 位图为 3,217 页，DMA-BUF 仍为零，VENC CRG 与中断计数保持为编码结束时的值。两轮控制项烟测分别增加 8 次 VENC IRQ 和排空，两轮结束时累计 976 次，SMMU fault 与 INTID 181 仍为 0。 [资源计数](logs/536-venc-ippp-result.json)

双 PP Lima 分别核对 PP0 和 PP1 各 16 个作业及交替模式 32 个作业，所有像素一致。Mesa GLES 离屏 60 帧通过，INTID 126 增量为 179。1080p JPGD 解码通过，INTID 129 +1。2160p60 Main10 源解码和 VDP 上屏均为 1,080 帧，零丢帧、零漏 vblank，HDMI 实际模式为 1920×1080@60。AIAO 48 kHz 播放 864,000 帧，零 XRUN。INTID 169 / 122 / 116 分别增加 1,080 / 1,090 / 1,800。 [媒体日志](logs/536-venc-ippp-acceptance.txt)

吞吐复测另产生 1,920 次 VENC IRQ 和排空，累计 2,896 次。复测结束时仍为零 SMMU fault、零 INTID 181、零 DMA-BUF，CMA 分配位图和 CRG 回到复测前的值。

覆盖 GOP=1 与 GOP=30。测试包括正常流重启，未注入总线故障触发隔离分支。

## 复现 IPPP

在远端保留已验收的 `out/lima-dualpp/source`、`out/lima-dualpp/final`、AIAO RAM initramfs 和 GCC 14.2.0 交叉工具链。以下命令会上传输入，并在新建的内核输出目录构建。

```powershell
$hgKey = (Get-ChildItem -LiteralPath $env:TEMP -Filter 'hg680kd-ssh-*.dpapi' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
$root = '/home/hedc/hg680kd-mainline'
$work = "$root/out/venc-ippp"
$inputDir = "$work/input"
& scripts/remote.ps1 -PasswordFile $hgKey run "mkdir -p '$inputDir'"
$inputs = @((Get-ChildItem src -Filter 'hi3798mv320-fiberhome-hg680kd-arm64*.dts').FullName) + @(
    'src/hi3798mv320-jpgd.dtsi', 'src/crg-hi3798mv320-reset.c',
    'src/hisilicon,hi3798mv320-jpgd.h', 'src/hisilicon,hi3798mv320-venc.yaml',
    'src/hi3798mv320-venc.c', 'src/hi3798mv320-venc-r002.h', 'src/venc-test.c',
    'patches/0019-drm-lima-rearm-shared-pp-broadcast-irq.patch',
    'patches/0020-media-add-Hi3798MV320-venc.patch',
    'scripts/build_venc_736.sh', 'scripts/pack_arm64.py'
)
& scripts/remote.ps1 -PasswordFile $hgKey put-inputs $inputDir @inputs
& scripts/remote.ps1 -PasswordFile $hgKey run ('VENC_WORK_DIR={0} VENC_BUILD_DIR=$(mktemp -d "{0}/clean.XXXXXXXX") bash {0}/input/build_venc_736.sh {1}' -f $work, $root)

$dir = 'artifacts/arm64-7.3.6-venc-ippp'
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($name in @('boot.img', 'venc-test', 'SHA256SUMS', 'build.log', 'kernel.config',
    'kernel.release', 'hi3798mv320-fiberhome-hg680kd-arm64-venc.dtb')) {
    & scripts/remote.ps1 -PasswordFile $hgKey get "$work/release/$name" "$dir/$name"
}
python -u scripts/venc_ram_test.py $dir --frames 120 --gops 1 30 --log "$dir/retest-com4.txt"
```

复测需要 pyserial、NumPy、FFmpeg / ffprobe、已校验的原厂 BL31 RAM 副本，以及双 PP Lima、JPGD 和 2160p60 播放的既有验证件。板端可直接用 `venc-test --gop 30 --i-qp 32 --p-qp 32 /dev/video2 1920 1080 NV12 120 input.raw output.h264` 编码紧凑排列的可见 NV12 帧。`--input-fps` 指定码率换算所用的源帧率，默认 30。
