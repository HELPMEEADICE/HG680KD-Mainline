# HG680KD Linux 7.3.6 USB 最终版

## 构建与部署

USB 版从 SHA256 已核对的 AIAO drain RAM 验收快照 `clean.3AlNpmSD` 构建。GCC 14.2.0 使用空输出目录 `out/arm64-7.3.6-usb-final/clean.8uLxORWb/build`，内核与 DTB 零警告。构建脚本逐字节核对仓库中的驱动与验收源码树，并重编译 RAM DTB 进行同字节校验。 [构建脚本](scripts/build_usb_final_736.sh) · [RAM 验收](HDMI-DRM-ACCEPTANCE.md#aiao-drain-修复复验)

USB DTB 继承原生 HDMI、双 PP Mali-450、VDEC、JPGD、VENC、AIAO 和 KVM 的板级配置。启动参数保留 `root=PARTLABEL=HG680KD_ROOT`、`rootfstype=f2fs`、原有 F2FS 挂载选项、`rootwait` 和 `cma=384M`。配置内建 USB Mass Storage 和所需的文件系统，关闭模块、MMC、MTD、NVMEM 和 `/dev/mem`。USB PHY 使用默认 trim，DTB 只映射 xHCI 和两路 PHY。 [USB DTS](src/hi3798mv320-fiberhome-hg680kd-arm64-usb-final.dts) · [最终内核配置](src/arm64-7.3.6-usb-final.config)

| USB 路径 | SHA256 |
| --- | --- |
| `/boot/boot.img` | `6fddb34f1d0c5bff8ff663251e77ec2998601c19b4166dedee7588c416fb34b1` |
| `/boot/boot.img.bak` | `275642c781c1294e36d0ba4639eff98536d47b9cb77f26e56fbf65a0edd064ba` |
| `/opt/vdp-player/vdp-player` | `98a902e6885ffdcaf9428b7bfb9cce6db98cecd521ccd9989428c0c3fb656fcb` |
| `/opt/hg680kd-mpv-native-drain/usr/bin/mpv` | `f9ee7969f6abc0b0c7cdd3f2149f6957e0b85544e0f61b1ca140db42631d3925` |
| `/usr/local/bin/mpv` | `8475dc354c28de4c3c5dd89411c3c5957e9c3586c92226c78b0059194b3e2af1` |

本地镜像位于 `artifacts/arm64-7.3.6-usb-final/boot.img`。14,645,320 字节的有效 uImage 补齐到 FAT 文件的原有长度 15,245,384 字节。部署在写入前核对 SanDisk USB 传输类型及 VFAT、F2FS 两个分区的 PARTUUID。安装脚本原位更新启动文件，卸载后对 LBA 4136 起的 29,777 个扇区做直接 I/O 回读。镜像有效文件范围逐字节一致，窗口尾部 440 字节与写入前一致，完整窗口 SHA256 为 `f6b4f1849173d164d7e35ee66e0ee8775bdad2b410f96bc0a6fb7428186f95b3`。重新只读挂载后再次核对新镜像和备份，再同步并卸载。 [安装脚本](scripts/install_usb_final_736.sh) · [U-Boot 读盘与回退](UBOOT-USB-BOOT.md)

`vdp-player` 权限为 755，入口为 `/opt/vdp-player/run`。原生 drain 版 MPV 的入口为 `/usr/local/bin/mpv`。FFmpeg Request 的配套命令是 `/usr/local/bin/ffmpeg-v4l2request` 和 `/usr/local/bin/ffprobe-v4l2request`。三个入口使用同一套私有 musl 运行时，FFmpeg 共享库位于 `/opt/hg680kd-mpv-native-drain/usr/lib`。安装时执行 `ldconfig`，冷启后以 musl `ldd` 核实三个 ELF 的动态库均可解析。MPV 和 FFmpeg 均报告 V4L2 Request 支持。 [MPV drain 补丁](scripts/mpv-alsa-native-drain.patch) · [FFmpeg 及 MPV 构建说明](MPV-V4L2-DRM-PLAN.md)

## 物理冷启动

COM4 以 115200 8N1 独占记录安全关机、BootROM、U-Boot 固定扇区读取和 Linux 启动。新 boot ID 为 `ff9de364-9b8c-4c5a-8722-ecb9e63bc87f`。根分区为 `/dev/sda3` F2FS，启动分区为 `/dev/sda1` VFAT，`CmaTotal` 为 393,216 KiB，运行配置与构建 SHA256 一致。KVM 已内建，原厂 BL31 默认交接在 EL1，日志报告 `HYP mode not available`。部署期间无 eMMC 写入、OTP 操作和 `saveenv`。

### HDMI、AIAO 与 MPV

`card0-HDMI-A-1/status` 为 `connected`，sysfs 返回 256 字节 EDID，与 RAM 验收时 MS2131 的 EDID 一致。HPD 注册在 GIC INTID 120。下列切换在保持同一 PCM 流开启时完成，模式同时由 KMS、VT1 和 MS2131 接收端验证。32 秒内 AIAO 播放 1,536,000 帧 48 kHz PCM，XRUN 为 0。

| 模式顺序 | 接收端 VIC | 接收端刷新率 | VT1 实测 |
| --- | ---: | ---: | ---: |
| 1080p60 | 16 | 60.000 Hz | 59.999970 Hz |
| 2160p30 | 95 | 30.000 Hz | 29.999925 Hz |
| 2160p60 | 97 | 60.000 Hz | 60.000000 Hz |
| 1080p60 | 16 | 60.000 Hz | 59.999940 Hz |
| 2160p60 | 97 | 60.000 Hz | 59.999910 Hz |

通过轮使用原生 2160p60、`--vo=gpu --gpu-context=drm`、80 ms / 8 periods。V4L2 Request 解出 1,080 帧 Main10，经 V0 的四平面 H0TV DRM PRIME 上屏 1,080 帧，实测 **60.000 FPS**、`detile=0`。VO 与解码掉帧、漏 vblank、MSC/时间戳差异、XRUN 和 drain timeout 均为 0。ALSA 以 3,840 帧为缓冲、480 帧为周期，记录一次 `ALSA PCM drain completed`，进程退出码为 0。退出前后的 KMS 平面和模式一致，DMA-BUF 归零，CMA 使用量从 5,258 页降到 5,242 页。

复测保留了达到门槛和未达到门槛的日志。首轮出现 60.001 FPS，随后两轮达到 60.000 FPS。额外重复播放中有两次各漏 1 个 vblank，所有 MPV 轮次的 ALSA drain 均成功。HDMI、MPV 和后续媒体回归使用同一次物理冷启动的 boot ID。 [板端 HDMI 与 MPV 命令](scripts/verify_usb_hdmi_736.sh) · [MPV 指标解析](scripts/mpv_usb_metrics.py)

### 桌面、GPU、媒体与资源

| 项目 | 冷启实测 |
| --- | --- |
| 控制台和桌面 | fbcon、LightDM 和 Xfce 恢复到 1080p60，HDMI 采集确认桌面显示。 |
| Mali-450 | `renderD128` 由 Lima 提供，Mesa GLX 和 EGL/GLES 使用 Mali450；glmark2 三个场景完成，16 个单 PP 和 16 个双 PP 作业均匹配 4,096/4,096 像素。 |
| 新版 `vdp-player` / HEVC | G0 Coverage OSD 翻转 100 次，V0 导入 4K Main10 H0TV。1080p60 稳态档位显示 1,080/1,080 帧，60.000 FPS，零掉帧、零漏 vblank。AIAO 播放 864,000 帧，XRUN 为 0。 |
| H.264 VDEC | 1080p60 显示 1,080/1,080 帧，60.000 FPS，零掉帧、零漏 vblank。 |
| MPEG-1 / MPEG-2 VDEC | 两种 1080p 流各解码 120/120 帧，113.262 / 116.486 FPS，VDH IRQ 各增加 120。 |
| JPGD | `/dev/video1` 解出 1920×1080 JPEG，IRQ 增加 1。 |
| 标准 FFmpeg IPPP VENC | `h264_v4l2m2m` 使用 `/dev/video2`，输出 1920×1080 的 1 I + 11 P，VENC IRQ 增加 12。输出通过主机 FFmpeg 严格回解，SHA256 为 `1b620066dc12960023da72e612fb5e6603c93163c60808703a0a5d0060b22dd5`。 |
| 故障计数和资源 | VDP underflow、fetch underflow、VENC SMMU fault、INTID 181 均为 0。DMA-BUF 回到 0 对象、0 字节。CMA 用量恢复到 3,225 页，`CmaFree` 从 368,440 到 379,076 KiB，VENC CRG 回到原值。 |

首轮 `vdp-player` 在 2160p60 切回其首选的 1080p60 时漏了 2 个 vblank。后续先恢复 1080p60 控制台再播放，完整通过。最终 `dmesg` 无媒体、Lima、SMMU 或 USB I/O 错误，保留已有的 `tmp.mount` / zram 启动排序告警。 [双 PP](LIMA-DUALPP.md) · [四格式 VDEC](VDEC-EXT-CODECS.md) · [JPGD](JPGD-ACCEPTANCE.md) · [FFmpeg VENC](VENC-ACCEPTANCE.md) · [板端验收入口](scripts/verify_usb_736.sh)

复验时将测试素材、`modetest`、`drm-vblank-measure`、`verify_usb_hdmi_736.sh`、`mpv_usb_metrics.py` 与其 `TEST-SHA256SUMS` 放到板端 tmpfs。以 root 在该目录运行 `bash verify_usb_736.sh cold-final BOOT_SHA256 PLAYER_SHA256 CONFIG_SHA256 MPV_SHA256`，替换为对应文件的实际 SHA256。独立重跑桌面与媒体回归时可使用 `cold-final-media`。完整构建、部署和串口记录保存在本地 `artifacts/arm64-7.3.6-usb-final/`，通过的 HDMI 与 MPV 数据在 `accept-cold5/`，媒体回归在 `accept-coldmedia/`。
