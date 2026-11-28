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

## 复现

在远端保留已验收的 `out/lima-dualpp/source`、`out/lima-dualpp/final`、AIAO RAM initramfs 和 GCC 14.2.0 交叉工具链。以下命令会上传输入，并在新建的内核输出目录构建。

```powershell
$hgKey = (Get-ChildItem -LiteralPath $env:TEMP -Filter 'hg680kd-ssh-*.dpapi' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
$root = '/home/hedc/hg680kd-mainline'
$inputDir = "$root/out/venc/input"
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
& scripts/remote.ps1 -PasswordFile $hgKey run 'cd /home/hedc/hg680kd-mainline && VENC_BUILD_DIR=$(mktemp -d "$PWD/out/venc/clean.XXXXXXXX") bash out/venc/input/build_venc_736.sh "$PWD"'

$dir = 'artifacts/arm64-7.3.6-venc'
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($name in @('boot.img', 'venc-test', 'SHA256SUMS', 'build.log', 'kernel.config',
    'kernel.release', 'hi3798mv320-fiberhome-hg680kd-arm64-venc.dtb')) {
    & scripts/remote.ps1 -PasswordFile $hgKey get "$root/out/venc/release/$name" "$dir/$name"
}
python -u scripts/venc_ram_test.py $dir --log "$dir/retest-com4.txt"
```

复测需要 pyserial、NumPy、FFmpeg、已校验的原厂 BL31 RAM 副本，以及双 PP Lima、JPGD 和 2160p60 播放的既有验证件。板端可直接用 `venc-test /dev/video2 1920 1080 NV12 32 60 input.raw output.h264` 编码紧凑排列的可见 NV12 帧。
