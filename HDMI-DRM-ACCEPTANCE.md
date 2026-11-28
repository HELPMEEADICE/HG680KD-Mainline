# HG680KD 原生 HDMI DRM RAM 验收

测试日期为 2026-11-28。目标是基于全媒体 Linux 7.3.6 运行内建 `hi3798mv320-vdp`，通过 HDMI-A connector 和 Atomic KMS 驱动三档 SDR 输出。HPD、DDC、TX、PHY 和 ACR 由 DRM 驱动管理，ALSA AIAO 负责 TX2 的 48 kHz S16_LE PCM。G0 为 primary，V0 为支持 10-bit Tiled DMA-BUF 的 overlay。 [接管设计](HDMI-TX-PLAN.md#10-内建-drm-驱动接管)

## 构建与 RAM 引导

| 项目 | 实测 |
| --- | --- |
| 构建机 | `hedc@ArchLinux-E5-Server:/home/hedc/hg680kd-mainline` |
| 内核 | `7.3.6-hg680kd-arm64-hdmi-drm` |
| 构建目录 | `out/arm64-7.3.6-hdmi-drm/clean.dFVfes5N/build`，开始时为空 |
| 编译 | AArch64 GCC 14.2.0，`CONFIG_WERROR=y`，内核与 DTB 零警告 |
| 镜像大小 | 19,626,056 字节 |
| 镜像 SHA256 | `570086220b4454e4e361170575fbee11b63e7c16e972aabdcdff93a52858ecee` |
| U-Boot RAM 回读 CRC32 | 镜像 `2e7ed6c1`，BL31 `fa236592` |

SSH 使用 `scripts/remote.ps1` 与现有 DPAPI 凭据。Windows 主机名解析失败时，以 `--address 192.168.6.160` 建连并按 `archlinux-e5-server` 的固定主机密钥验证。内核配置内建 DRM、AIAO、Lima、VDEC、JPGD 和 VENC，关闭模块、MMC、MTD、USB Mass Storage 和 UAS。 [构建脚本](scripts/build_hdmi_drm_736.sh) · [完整构建日志](artifacts/arm64-7.3.6-hdmi-drm/build.log) · [最终配置](artifacts/arm64-7.3.6-hdmi-drm/kernel.config) · [镜像](artifacts/arm64-7.3.6-hdmi-drm/boot.img) · [SHA256 清单](artifacts/arm64-7.3.6-hdmi-drm/SHA256SUMS)

COM4 以 115200 8N1 单进程独占。U-Boot 通过 TFTP 载入 BL31 到 `0x08020000`、镜像到 `0x01fdffc0`，回读两段 RAM CRC 后执行 `bootm 1fdffc0`。Linux 启动于内置 `rootfs`，`/proc/partitions` 无块设备。整个流程的 U 盘写入、eMMC 写入、OTP 操作和 `saveenv` 次数均为零。 [COM4 日志](artifacts/arm64-7.3.6-hdmi-drm/board-com4.txt)

## DRM、EDID 与接收端

`card0-HDMI-A-1/status` 返回 `connected`，`edid` 读出 256 字节。基础块及 CTA-861 扩展块均通过校验，内容与独立探针读出的 MS2131 EDID 相同，SHA256 为 `b2538071e7ee7a08a2da912419313326b955233d2204894227c984e6c11a060f`。标准 `modetest -a -c -p` 查询显示 148.5、297 和 594 MHz 的像素时钟。EDID 宣告的 TMDS 上限为 300 MHz，VIC 97 选择 YCbCr 4:2:0 8 bpc，对应 297 MHz TMDS。八次连续 `GETCONNECTOR` 均保留 UHD60。GIC INTID 120 注册为 `hi3798mv320-hpd`。 [KMS 枚举](artifacts/arm64-7.3.6-hdmi-drm/drm-inventory.txt) · [重复查询](artifacts/arm64-7.3.6-hdmi-drm/edid-reprobe.txt) · [EDID](artifacts/arm64-7.3.6-hdmi-drm/edid.bin)

标准 libdrm 2.4.131 `modetest` 使用 `-M hi3798mv320-vdp -a -s ... -P ...` 切档。2160p60 指定 G0 源为 1920×1080、目标为 3840×2160，避免已在探针阶段确认的全宽 ARGB 取数 underflow。VT1 由 DRM vblank 时间戳计算，HPLL / PHY 和 ACR 由 RAM 配置的只读 MMIO 工具采样。MS2131 通过 HID 独立回读原生时序、AVI、GCP、SDR InfoFrame 和 ACR，DirectShow 采集用于检查彩条与双声道，KMS 模式切换期间由 HDMI 链路正常重新锁定。 [验收脚本](scripts/hdmi_drm_ram.py) · [DRM 和寄存器采样](scripts/drm_vblank_measure.c) · [接收端回读](scripts/hdmi_receiver.py)

| 顺序 | 原生输出 | VIC | RX ACR 刷新率 | VT1 频率 | N / SW CTS / HW CTS | 彩条帧 | PCM XRUN |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: |
| 1 | 1080p60 RGB8 | 16 | 60.000 Hz | 59.999940 Hz | 6144 / 148500 / 148500 | 262/262 | 0 |
| 2 | 2160p30 RGB8 | 95 | 30.000 Hz | 29.999993 Hz | 6144 / 297000 / 297000 | 262/262 | 0 |
| 3 | 2160p60 YCbCr 4:2:0 8 bpc | 97 | 60.000 Hz | 59.999880 Hz | 6144 / 297000 / 297000 | 262/262 | 0 |
| 4 | 1080p60 RGB8 | 16 | 60.000 Hz | 60.000000 Hz | 6144 / 148500 / 148500 | 262/262 | 0 |
| 5 | 2160p60 YCbCr 4:2:0 8 bpc | 97 | 60.000 Hz | 60.000030 Hz | 6144 / 297000 / 297000 | 262/262 | 0 |

五档的 AVI 与 SDR InfoFrame 校验和均有效，GCP CD=4，SDR EOTF=0。HPLL 与 PHY TXPLL 锁定位均为 1，DDC 请求、应答和仲裁归零，TX 加扰位为 0。1,310 帧稳态彩条均通过颜色与完整性采样，每档播放 288,000 帧 48 kHz 立体声 PCM，采集两声道均为 1 kHz。随后保持同一 ALSA PCM 开启，在 2160p60 → 1080p60 → 2160p60 切换期间连续播放 576,000 帧，PCM 状态持续为 RUNNING，XRUN 为 0。 [逐档数据](artifacts/arm64-7.3.6-hdmi-drm/acceptance.json) · [连续 PCM 串口记录](artifacts/arm64-7.3.6-hdmi-drm/board-com4.txt)

## MPV 4K60 零拷贝

保留 `modetest` 原生 2160p60 状态，使用基线 MPV 0.41.0 的 V4L2 Request 构建，以 `--vo=gpu --gpu-context=drm` 播放同一段 18 秒 HEVC Main10 + LPCM。MPV 指定 `--drm-mode=3840x2160@60 --drm-draw-surface-size=1920x1080`。通过的一轮使用 `--video-sync=display-resample --alsa-buffer-time=80000 --alsa-periods=8`，ALSA 协商为 3,840 帧缓冲与 480 帧周期。CPU governor 在复测期间设为 `performance`，退出后恢复 `schedutil`。 [通过轮完整命令与统计](artifacts/arm64-7.3.6-hdmi-drm/mpv-10ms-period/acceptance.json)

V0 直接导入四平面 `H0TV` DMA-BUF，modifier 为 `0xfe00000000000320`。VDEC 记录 `detile=0`。提交和上屏均为 1,080 帧，实际 60.000 FPS，VO 与解码掉帧均为 0，漏 vblank 与 MSC/时间戳差异均为 0，ALSA XRUN 为 0。MPV 退出恢复运行前的 G0/V0 状态，关闭测试客户端后恢复 1080p60 fbcon。早期试跑出现起播漏 vblank，旧镜像也有一次退出 XRUN。通过轮的退出日志有一次 ALSA `drain timeout`，进程退出码为 0，控制台恢复成功。 [通过轮日志](artifacts/arm64-7.3.6-hdmi-drm/mpv-10ms-period/main10-av-attempt2.mpv.log) · [VDEC 计时](artifacts/arm64-7.3.6-hdmi-drm/mpv-10ms-period/mpv-vdec-timing.txt) · [各次尝试](artifacts/arm64-7.3.6-hdmi-drm/acceptance.json)

## 全媒体回归

| 项目 | 实测 |
| --- | --- |
| Lima 双 PP | PP0 与 PP1 各 16/16 个作业匹配参考像素，双 PP 32/32 个匹配。INTID 126 增量为 32、32、64。Mesa GLES 60/60 帧通过，INTID 126 增量 180。 |
| H.264 VDEC | 1,080/1,080 帧，60.000 FPS，零掉帧、零漏 vblank。 |
| HEVC Main10 VDEC / VDP / AIAO | 参考播放器经 V0 扫描 1,080/1,080 帧，60.000 FPS，零掉帧、零漏 vblank，48 kHz PCM 播放 864,000 帧且零 XRUN。 |
| MPEG-1 VDEC | 1080p 120/120 帧，VDH IRQ +120，逐帧最小 Y/U/V PSNR 为 68.15 / 63.69 / 64.12 dB。 |
| MPEG-2 VDEC | 1080p 120/120 帧，VDH IRQ +120，逐帧最小 Y/U/V PSNR 为 70.58 / 67.08 / 67.64 dB。 |
| JPGD `/dev/video1` | 1920×1080 JPEG 成功解码，INTID 129 增量 1。 |
| FFmpeg VENC `/dev/video2` | 标准 `h264_v4l2m2m` 编码 1080p NV12 的 120 帧，GOP 30，VENC IRQ +120。MP4 时间戳及 GOP 校验通过，软件和 VDEC 均回解 120 帧，VENC SMMU fault 为 0。 |

[串口实测](artifacts/arm64-7.3.6-hdmi-drm/board-com4.txt) · [逐项数据](artifacts/arm64-7.3.6-hdmi-drm/acceptance.json)

## 退出恢复与资源

VDP underflow、fetch underflow、VENC SMMU fault 和 GIC INTID 181 的计数均为 0。DMA-BUF 回到 0 对象、0 字节。CMA 使用量回到温机基线 3,217 页，`CmaFree` 从 380,312 到 380,168 KiB。冷机初值为 3,193 页，首次 GPU 播放后增加 24 页，后续媒体作业退出后稳定在 3,217 页，与基线全媒体 RAM 验收的收尾值一致。 [最终寄存器与挂载](artifacts/arm64-7.3.6-hdmi-drm/final.txt) · [控制台平面](artifacts/arm64-7.3.6-hdmi-drm/console-restored.txt) · [7.3.6 RAM 参考](logs/538-vdec-ext-acceptance.txt)

本轮覆盖 MS2131 的三档 SDR 与 HPD IRQ 注册。物理拔插、594 MHz RGB8 线端及 Xorg 的 `xrandr` 未列入本次执行。
