# Hi3798MV320 HDMI 音频

基线为 `7.3.6-hg680kd-arm64-vdp`（`7c92d99`）。音频测试目标 `7.3.6-aiao` 继承已验收的内核配置和 VDEC/VDP 驱动，通过独立 DTB 注册 ALSA PCM。 [7.3.6 基线验收](logs/491-vdp-7.3.6-phase3-acceptance.txt)

## 原厂证据与板上回读

原厂 `arm32-result.txt` 的选符范围覆盖 VDEC 和内存管理。AIAO 与 HDMI 音频的位域用同一原厂 ARM32 内核和 kallsyms 交叉核对。原厂 DTB 的 `/soc/virtdev` 把 `aiao` 和 `<0 84 4>` 配对。原厂 `hal_aiao_request_isr` 传入 `0x74`，即 GIC INTID 116。 [原厂反编译入口](artifacts/vpu-reverse-factory/arm32-result.txt) · [kallsyms](artifacts/vpu-reverse-factory/hg680kd-stock-kallsyms.txt) · [原厂 DTB](artifacts/vpu-reverse-factory/inputs/factory.dtb) · [海思芯片规格](https://www.hisilicon.com/cn/products/smart-media/STB/Hi3798MV320)

| 区域 | 寄存器与原厂依据 |
| --- | --- |
| AIAO 总线 | `aiao_hw_init`（`c0711224`）映射 `0xf8cd0000`，长度 `0x10000`。全局 IRQ 使能、状态和原始状态为 `+0x00/04/08`，复位就绪为 `+0x30[0]`，DMA burst 字段为 `+0x34[2:0]=2`。 |
| 时钟与复位 | `aiao_hw_power_on`（`c071101c`）使用 CRG `0xf8a22000+0x118`。bit 0 为门控，bit 4 为软复位，bit 22/17 置位并清零 `[16:12]` 选择原厂时钟域。U-Boot 回读 `0x00420001`，复位就绪为 1。 |
| HDMI I2S 输入 | `hdmi_op_get_hw_param`（`c072e594`）为 LPCM 选原厂端口 `0x12`，即 TX2，端口 MMIO 为 `0xf8cd2200`。另一输出路径使用 SPDIF `0x20`。48 kHz 的 TX2 MCLK 和时钟字段回读为 `0xf8cd0150=0x234f72`、`0xf8cd0154=0x133`。每帧 64 个 BCLK，MCLK/BCLK 为 4，即 BCLK 3.072 MHz、MCLK 12.288 MHz。TX2 接口字段回读 `0xe4280014`。 |
| TX2 DMA | 端口 `+0x80` 为 32 位 DDR DMA 基址，`+0x84[23:0]` 为环长，`+0x88/+0x8c` 为字节写/读指针。`+0x90/+0x94` 为水位和周期字节数。源自 `aiao_hw_set_buf_addr_and_size`、`aiao_hw_set_buf_{rptr,wptr}`、`aiao_hw_set_buf_{threshold,period}_size`。 |
| TX2 中断与启停 | 全局 bit 18 汇总 TX2。端口 `+0xa0/a4/a8/ac` 分别为使能、原始状态、屏蔽后状态、W1C。原厂 `i_hal_aiao_open` 使能 bit 0 的周期中断。端口控制 `+0x04[28]` 为 START，bit 29 为 STOP_DONE，bit 0 为端口静音。 |

HDMI TX 保持 U-Boot 的 VIC 97、3840×2160p60、YCbCr 4:2:0、8-bit SDR。像素时钟 594 MHz，TMDS 字符率 297 MHz。48 kHz LPCM 使用 `N=6144`，按 `CTS = TMDS × N / (128 × Fs)` 得 `CTS=297000`。U-Boot 已写入这两个值并打开 I2S 和 Audio InfoFrame，TX2 的 DMA 和 IRQ 寄存器仍为零。出声需要内核提供 TX2 样本、DMA 和中断，驱动同时校验并恢复已知的 HDMI 音频字段。 [U-Boot 寄存器实读](artifacts/arm64-7.3.6-aiao/firmware-audio.txt) · [视频交接记录](HDMI-REVERSE.md)

| TX 窗口 | 音频字段 |
| --- | --- |
| `0xf8ce1000` 音频 | `+0x00[0]` 输入使能，`[1]` Audio Mute，`[2]` Layout，`[7:4]` I2S lane 使能，`[8]` SPDIF 使能。`+0x04[11:8]=2` 为 16-bit。IEC 60958 的 FS 和原始 FS/字长字段为 `+0x0c=0x02000000`、`+0x10[7:0]=0xd2`。 |
| `0xf8ce1000` ACR | `+0x40=7` 为固件已工作的 CTS 请求及选择模式，`+0x48/4c/50` 分别读写 N、软件 CTS、硬件 CTS。`hal_hdmi_ctrl_audiopath_set` 写入 N/CTS 后短脉冲 `0xf8ce0010[8:6]`，仅复位 Audio FIFO、ACR 和 Audio。 |
| `0xf8ce1800` 数据包 | `+0x3c=0x000a0184` 为 Audio InfoFrame 头，`+0x40=0x00000170` 为校验和及双声道字段。`+0x1ac[1:0]=3` 周期发送。GCP 的 `+0x20c[1:0]=2` 表示固件已发 Clear AVMUTE，播放启停使用独立 Audio Mute。 |

## 实现

- DT 通过 CRG syscon 管理 AIAO 门控和复位，声明 TX2 所属的 64 KiB AIAO 窗口、SPI 84 和三段 HDMI 音频相关窗口。音频 DT 把 VDP 的 128 KiB 资源收窄到实际使用的首个 64 KiB，解除与 AIAO 的资源重叠。 [音频 DT](src/hi3798mv320-fiberhome-hg680kd-arm64-aiao.dts)
- PCM 固定 48 kHz、S16LE、双声道，采用 ALSA 管理的 32 位一致性 DMA 缓冲。DDR 基址随分配写入 TX2。硬件以读写指针相等表示空环，整环提交时保留最多 32 字节，在周期中断中按更新后的硬件指针释放余量。STOP 先静音、关闭 DMA 和中断，然后等 STOP_DONE。超时仅复位 AIAO，避免残留 DMA 访问。 [ALSA 驱动](src/hi3798mv320-aiao.c)
- 播放器用独立音频 demux/解码线程解码 AAC 或 PCM，通过 libswresample 转 48 kHz S16LE。ALSA 用直接 ioctl 协商 10 ms 周期和 80 ms 环。首次 KMS 翻页完成后启动 ALSA。每次翻页用 ALSA 硬件指针推算该 vblank 对应的音频 PTS，再低通修正后续相位。Seek 等待已提交翻页完成，停止音频线程、执行 DROP、清理音视频预取和解码状态。音频从目标前 250 ms 内开始寻址，再按采样帧裁到新段首帧 PTS。已运行的 PCM 在恢复时先发送 15 个刷新周期的静音样本，60 Hz 下为 250 ms。新段首帧与静音结束对齐，媒体时钟扣除这段样本。旧的扫描缓冲在替换帧完成后释放。 [音频线程](src/vdp-player-audio.c) · [播放和翻页](src/vdp-player.c)

## 构建与复现

远端构建使用以下目标，产物在 `out/release-arm64-7.3.6-aiao/` 和 `out/vdp-player/serve/`。

```sh
VDEC_TEST=1 VDEC_FIXTURES='h265ip1080 hevc10ip1080 hevc4k hevc10ip4k ip32' \
  bash scripts/build_arm64.sh /home/hedc/hg680kd-mainline inputs 7.3.6-aiao
bash scripts/build_vdec_ffmpeg.sh /home/hedc/hg680kd-mainline player
```

板端测试使用 `scripts/vdp_player_ram.py --av --image <boot.img> --payload <serve> --log <board.txt>`。双路采集传入 `--capture-device` 和 `--capture-audio`。样本 `main10-av.mkv` 和 `h264-av.mp4` 都是 18 秒、60 fps，1 至 17 秒每秒同步出现 100 ms 白框和 1 kHz 音频脉冲。Main10 使用 48 kHz PCM，H.264 使用 48 kHz AAC-LC。脚本从 RAM 运行 5 秒独立测试音、两个 EOS、三次 Seek 和三轮独立起停。线性 NV21/P010 用 `scripts/vdec_ffmpeg_ram.py --mode check` 在同一 RAM 系统回归。 [RAM 测试脚本](scripts/vdp_player_ram.py) · [线性回归脚本](scripts/vdec_ffmpeg_ram.py)

## 2026-11-27 实机结果

远端 AArch64 GCC 14.2.0 内核构建零警告。ALSA、AIAO、VDEC 和 VDP 内建，`CONFIG_MODULES` 关闭。COM4 以 115200 8N1 单进程独占。U-Boot TFTP 将镜像装到 `0x01fdffc0`，校验后执行 `bootm`。镜像 SHA256 为 `890e329cf7376a18f6814e8ed400935bc2dfc43e40a42d601656b391f1732fa6`，U-Boot RAM 回读 CRC32 为 `76c87e43`。系统根为 `rootfs`，eMMC 为只读，块设备挂载数为零。 [镜像](artifacts/arm64-7.3.6-aiao/boot.img) · [内核构建](artifacts/arm64-7.3.6-aiao/build.txt) · [播放器构建](artifacts/arm64-7.3.6-aiao/player-build.txt) · [COM4 RAM 回读](artifacts/arm64-7.3.6-aiao/acceptance-preroll-board.txt) · [最终播放验收](artifacts/arm64-7.3.6-aiao/delivery-board.txt)

`/dev/snd/pcmC0D0p` 用 5 秒 1 kHz、48 kHz S16LE 立体声独立播放。ALSA 走完 240,000 帧，XRUN 为 0，AIAO INTID 116 增加 501 次。USB 音频端采到 48 kHz 双声道，两个声道均测得 1000.0119 Hz。稳定区的 2 ms 窗口无中断，录音链路测得约 34.2 dB SINAD。 [录音](artifacts/arm64-7.3.6-aiao/tone.wav) · [采集分析](artifacts/arm64-7.3.6-aiao/capture-analysis-final.txt)

| 18 秒样本 | 显示 | 板端 PTS 差均值及范围 | CPU0 / 1 / 2 / 3 活跃比例 | 峰值 RSS | AIAO INTID 116 增量 |
| --- | --- | --- | --- | --- | --- |
| 2160p60 HEVC Main10 + PCM | 1080/1080，60.000 FPS，丢帧与错失 vblank 均为 0 | −0.832 ms（−1.279 到 −0.423） | 7.87% / 8.86% / 8.37% / 7.49% | 89,660 KiB | +1800 |
| 1080p60 H.264 + AAC + G0 | 1080/1080，60.000 FPS，丢帧与错失 vblank 均为 0，G0 翻页 100 次 | −0.993 ms（−1.099 到 −0.925） | 7.40% / 11.99% / 11.05% / 11.77% | 24,860 KiB | +1800 |

板端 PTS 差为显示帧 PTS 减同一 vblank 推算的 ALSA 播放 PTS。USB 采集是 1080p30 MJPEG 和 48 kHz PCM，两段连续播放均匹配 17/17 个闪框/音脉冲。采集到的视频起点减音频起点分别为 42–45 ms（中位 44 ms）和 56–59 ms（中位 57 ms）。这些值包含采集器两路的相对延迟，采集链尚无独立的绝对延迟标定。 [Main10 HDMI 采集](artifacts/arm64-7.3.6-aiao/main10-av.mkv) · [H.264 加 G0 HDMI 采集](artifacts/arm64-7.3.6-aiao/h264-av-osd.mkv)

三次 Seek 后正常 EOS，480 帧显示，0 丢帧、0 错失 vblank，板端 PTS 差为 −1.445 到 −0.470 ms。三次独立起停各显示 120 帧，均为 60.000 FPS。所有播放结束时 PCM 为 `closed`，DMA-BUF 为 0 对象、0 字节。Seek 的 CMA 回收后空闲量比开始多 28 KiB，三次独立起停的 CMA 空闲量变化依次为 −32、0、0 KiB。连续播放结束的 CMA 波动为 28–32 KiB。VDP underflow、fetch underflow 与 SMMU INTID 181 全程为零。在线性 NV21 和 P010 路径各比对 240 帧，均匹配软件解码参考。 [最终播放验收](artifacts/arm64-7.3.6-aiao/delivery-board.txt) · [线性逐帧比对](artifacts/arm64-7.3.6-aiao/linear.txt)

直接从 16.000 秒起播时，HDMI 采集匹配 2/2 个节拍。连续三次 Seek 的早期试验中，最后一次跳到 16.000 秒时，USB 录音未检出同点的首个 100 ms 音脉冲，下一拍恢复。提前 500 ms 定位的对照匹配 7/7。仅延长静音停机间隔后，该脉冲仍只录到约 10 ms。发送 150 ms 静音 PCM 的试验录到了 62 ms。 [独立起播采集分析](artifacts/arm64-7.3.6-aiao/start-capture-analysis.txt) · [Seek 前后对照](artifacts/arm64-7.3.6-aiao/seek-capture-analysis.txt) · [150 ms 复查](artifacts/arm64-7.3.6-aiao/warmup-capture-analysis.txt)

最终版用 250 ms 静音 PCM 恢复后，三次精确 Seek 的 HDMI 采集匹配 7/7 个节拍，7 段音脉冲实测均为 100–102 ms。跳转到 16.000 秒的音脉冲为 100 ms。 [最终 Seek 采集](artifacts/arm64-7.3.6-aiao/main10-av-seek.mkv) · [最终采集分析](artifacts/arm64-7.3.6-aiao/capture-analysis-final.txt)
