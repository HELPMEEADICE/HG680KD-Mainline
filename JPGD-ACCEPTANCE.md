# HG680KD JPGD 硬件 JPEG 解码

`arm64-7.3.6-jpgd` 继承 Linux 7.3.6 AIAO RAM 基线。JPGD 与 VDEC、VDP、AIAO 都内建。正式镜像不包含上一轮的 `hi3798-codec-probe`。基础 DTS 描述 JPEG 核心和私有 SMMU，JPGD RAM 变体启用节点、接入 CRG 时钟和复位，并禁用 eMMC。内核禁用 USB 大容量存储和 UAS。

驱动使用 32-bit DMA 地址和现有 CMA。每次作业启动前打开 SMMU 与核心时钟，释放复位，配置私有 SMMU 与各 DMA 通道的旁路。完成、中止、超时均在归还缓冲前重新断言复位并关闭时钟。SPI 97 对应 GIC INTID 129。JPGD 私有 SMMU 原始故障状态在每个作业结束时读取，同时用 VDEC 的 INTID 181 计数检查共享媒体回归。

寄存器布局由同板原厂反编译确认，量化表、Huffman 表和物理旁路配置结合海思 SDK 核对。 [同板原厂证据](MEDIA-CODEC-AUDIT.md#2-原厂驱动证据) · [SDK 表编码](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/boot/product/driver/graphic/hard-dec/jpeg_drv_table.c) · [SDK 作业地址配置](https://github.com/JasonFreeLab/HiSTBLinuxV100R005C00SPC050/blob/master/source/boot/product/driver/graphic/hard-dec/jpeg_drv_setpara.c)

## 2026-11-28 实机验收

AArch64 GCC 14.2.0 从空输出目录构建，内核、DTB 和静态测试工具编译零警告。COM4 以 115200 8N1 单进程独占，U-Boot 纯 RAM TFTP 装载与 CRC32 回读通过。运行内核为 `7.3.6-hg680kd-arm64-jpgd`，根文件系统为 BusyBox initramfs，eMMC 和 USB 大容量存储未暴露为块设备。U 盘 `/boot/boot.img` 写入、eMMC 刷写、OTP 操作及 `saveenv` 执行次数均为 0。 [完整构建日志](logs/520-jpgd-7.3.6-build.txt) · [串口验收](logs/521-jpgd-7.3.6-acceptance.txt)

| 镜像 | SHA256 | U-Boot RAM 回读 CRC32 |
| --- | --- | --- |
| `boot.img` | `0b969830ef6b093327dd0fb145a4d65ce451d3480704ecf1101313610d913e66` | `5375513a` |

`/dev/video1` 注册为 JPGD，`/dev/video0`、`/dev/media0`、`/dev/dri/card0` 和 `/dev/snd/pcmC0D0p` 均通过检查。临时探针的 debugfs 入口已消失。验收前后分别完成一次 HEVC 1080p 解码上屏、播放 240,000 帧 48 kHz PCM。两次 VDEC PXP、VDP、AIAO 中断增量均为 `+1 / +32 / +500`，ALSA XRUN 均为 0。VDEC SMMU INTID 181 始终为 0。JPGD dmesg 无报错，基线已有的蓝牙 H5 短包提示仍可见。 [最终串口](logs/521-jpgd-7.3.6-acceptance.txt)

### 单帧画质

板端直接导出可见区域的全范围 8-bit YUV。FFmpeg 解码同一个 JPEG 得到 I420 参考，逐平面计算误差。每张图各产生一次 SPI 97 中断。

| 样本 | 输出 | PSNR Y / U / V（dB） | MAE Y / U / V |
| --- | --- | --- | --- |
| 1280×720 | NV12 | 76.860 / 74.235 / 74.646 | 0.0013 / 0.0025 / 0.0022 |
| 1920×1080 | NV12 | 78.055 / 75.004 / 75.640 | 0.0010 / 0.0021 / 0.0018 |
| 3840×2160 | NV12 | 78.226 / 75.587 / 76.318 | 0.0010 / 0.0018 / 0.0015 |
| 1920×1080 | NV21 | 78.055 / 75.004 / 75.640 | 0.0010 / 0.0021 / 0.0018 |
| 638×478，DRI=16 MCU | NV12 | 62.649 / 66.410 / 66.100 | 0.0353 / 0.0149 / 0.0160 |

1080p 的 CAPTURE 存储高度为 1088 行。DRI 样本的行步幅为 640 字节，存储高度为 480 行。去掉 EOI 的样本以错误状态归还，SPI 97 计数保持不变，随后连续解码成功。 [单帧、错误帧和连续解码](logs/521-jpgd-7.3.6-acceptance.txt#L486) · [DRI 与步幅复核](logs/522-jpgd-7.3.6-dri.txt)

### 1080p 连续解码

120 张不同的 JPEG 在两队列各使用 3 个缓冲，连续解码耗时 692.790 ms，平均 **173.213 FPS**，每帧平均 **5.773 ms**。平均排队到 DQBUF 延迟为 17.002 ms。吞吐统计包含板端提交和出队，样本在计时前载入 RAM，连续测试不导出 YUV。

CPU0–CPU3 占用为 5.80%、7.25%、0.00%、0.00%，四核平均 3.26%。SPI 97 增加 120 次，DMA-BUF 退出后为 0 对象、0 字节。CMA 总量 393,216 KiB，压测前后空闲量为 388,448 → 388,288 KiB，回归结束后恢复到整轮媒体测试前的 388,548 KiB。CRG `+0x07c` 和 `+0x334` 最终均回读为 `0x00000010`。 [吞吐与资源计数](logs/521-jpgd-7.3.6-acceptance.txt#L683)

首次实测暴露的 7.3.6 V4L2 文件句柄访问异常和 U/V 顺序错误均已修正。上述结果来自重新构建后的最终镜像。

## V4L2 接口

`/dev/video1` 使用 multiplanar M2M ioctl。OUTPUT 为单平面 `JPEG`，每个 MMAP 缓冲提交一个包含 DQT、DHT 和 EOI 的 8-bit Baseline DCT YCbCr 4:2:0 帧。先按 JPEG 可见宽高设置 OUTPUT 与 CAPTURE。应用按预计 JPEG 大小加 128 字节设置 OUTPUT `sizeimage`，`bytesused` 只填实际文件长度。改变分辨率时先停止流并释放两队列的缓冲。

CAPTURE 支持单平面 `NV12` 和 `NV21`，可 MMAP 或使用 DMA-BUF。用 `G_FMT` 获取按 64 字节对齐的行步幅及按 16 行对齐的存储高度，色度面自 `bytesperline * height` 起。使用 `G_SELECTION(COMPOSE)` 获取可见宽高。驱动按协商尺寸校验 JPEG 头，对拒绝的帧以 `V4L2_BUF_FLAG_ERROR` 归还两队列缓冲。

## 复现

在远端准备已经应用仓库板级补丁的 7.3.6 源码树、通过验收的 AIAO RAM 发布目录及 AArch64 musl 工具链。向远端输入目录上传 `src/`、`patches/` 中 JPGD 相关文件及基础 DTS、CRG 源码，使用以下目标构建。

```sh
bash scripts/build_arm64.sh /home/hedc/hg680kd-mainline inputs 7.3.6-jpgd
```

取回 `out/release-arm64-7.3.6-jpgd/` 中的 `boot.img`、`jpgd-test`、`SHA256SUMS` 和 `build.log`。主机需要 FFmpeg、NumPy 和 pyserial。以下命令生成三张 Baseline 4:2:0 JPEG、FFmpeg 软件参考和 120 帧长度前缀 JPEG 序列，再通过 COM4 独占运行验收。验收脚本从已校验的原厂 BL31 副本和 `boot.img` 做两次 TFTP RAM 装载，逐一回读 CRC32，然后执行 `bootm`。

```powershell
python scripts/jpgd_ram_test.py artifacts/arm64-7.3.6-jpgd --prepare
python scripts/jpgd_ram_test.py artifacts/arm64-7.3.6-jpgd
```

`jpgd-test` 也可直接在板端使用。导出的 YUV 紧凑排列，只包含可见像素。

```sh
jpgd-test /dev/video1 1920 1080 frame.jpg --yuv frame.nv12
jpgd-test /dev/video1 1920 1080 --packed 1080p-sequence.bin 120 --export
```
