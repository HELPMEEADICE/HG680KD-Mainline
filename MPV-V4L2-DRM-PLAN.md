# HG680KD MPV V4L2 Request 与 VDP 零拷贝

2026-11-28 · Linux 7.3.6 · COM4 TFTP RAM

## 基线与兼容性

COM4 在已部署的 `7.3.6-hg680kd-arm64-usb` 上检查了 PATH 和 `dpkg-query -W ffmpeg mpv`。两个程序均未安装。仓库已有基于 FFmpeg Request 的 `vdp-player` 和独立 VENC 测试版 FFmpeg。此次测试使用 [MPV v0.41.0](https://github.com/mpv-player/mpv/tree/v0.41.0) 的标准解复用、解码和 GPU VO，链接 [Kwiboo FFmpeg `b57fbbe`](https://github.com/Kwiboo/FFmpeg/tree/b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935)。该 FFmpeg 分支提供 `v4l2request` 设备类型和 `AV_PIX_FMT_DRM_PRIME` 输出。 [板端盘点和启动摘录](logs/540-mpv-v4l2-drm-acceptance.txt) · [全媒体 USB 基线](logs/539-usb-7.3.6-full.txt) · [FFmpeg 主线硬件设备表](https://github.com/FFmpeg/FFmpeg/blob/master/libavutil/hwcontext.c)

| OUTPUT | Request 控件 | 本板可用范围 |
| --- | --- | --- |
| `S264` / `V4L2_PIX_FMT_H264_SLICE` | `V4L2_CID_STATELESS_H264_{DECODE_MODE,START_CODE,SPS,PPS,SCALING_MATRIX,SLICE_PARAMS,DECODE_PARAMS,PRED_WEIGHTS}` | Slice-based、Annex B、8-bit 4:2:0。CAPTURE 为 NV21 或 H8TV。 |
| `S265` / `V4L2_PIX_FMT_HEVC_SLICE` | `V4L2_CID_STATELESS_HEVC_{DECODE_MODE,START_CODE,SPS,PPS,SCALING_MATRIX,SLICE_PARAMS,DECODE_PARAMS}` | Slice-based、Annex B，支持 Main 和 Main10。CAPTURE 可选 NV21、P010、H8TV、H0TV。与所选 FFmpeg 分支核对 entry-point 元数据和 WPP 行为。 |
| `MG2S` / `V4L2_PIX_FMT_MPEG2_SLICE` | `V4L2_CID_STATELESS_MPEG2_{SEQUENCE,PICTURE,QUANTISATION}` | Progressive 8-bit 4:2:0，1080p I/P/B。FFmpeg 将参考 CAPTURE 时间戳写入 `PICTURE`，按图片提交 slice 数据。CAPTURE 为 NV21 或 H8TV。 |
| `MPG1` / `V4L2_PIX_FMT_MPEG1` | Stateful M2M | 保持现有 MPEG-1 路径，FFmpeg Request 本轮覆盖 H.264、HEVC 和 MPEG-2。 |

标准调用顺序是枚举 Media Controller 和匹配的 V4L2 视频节点，`VIDIOC_QUERYCAP`、`VIDIOC_ENUM_FMT`、选择 OUTPUT 和码流尺寸，设置所需初始控件，再枚举并设置 CAPTURE。请求模式的 OUTPUT 队列提供 `V4L2_BUF_CAP_SUPPORTS_REQUESTS`。H.264 和 HEVC 多 slice 用 `V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF` 续写当前帧。FFmpeg 通过 `/dev/media0` 的 `MEDIA_IOC_REQUEST_ALLOC` 取 request fd，用 `VIDIOC_S_EXT_CTRLS`、`V4L2_CTRL_WHICH_REQUEST_VAL` 关联控件，CAPTURE 独立入队，OUTPUT 带 request fd 入队，再 `MEDIA_REQUEST_IOC_QUEUE`。完成后 DQBUF，CAPTURE 用 `VIDIOC_EXPBUF` 导出，用户态持有参考图和正在显示的输出直至可回收。 [Linux Stateless Decoder 接口](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html) · [Request API](https://docs.kernel.org/userspace-api/media/mediactl/request-api.html)

控件数字 ID 基于 `V4L2_CID_CODEC_STATELESS_BASE = 0x00a40900`。H.264 占偏移 `0–7`，MPEG-2 占 `220–222`，HEVC 已实现 `400–406`。HEVC `ENTRY_POINT_OFFSETS`（偏移 `407`）当前未注册。所选 FFmpeg 分支先查询该控件，再按查询结果启用对应的请求载荷。通用 profile/level 查询和交错 MPEG-2 支持也仍有限。 [控件 UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/v4l2-controls.h) · [FFmpeg HEVC Request](https://github.com/Kwiboo/FFmpeg/blob/b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935/libavcodec/v4l2_request_hevc.c) · [驱动](src/hi3798mv320-vdec.c) · [MPEG-1/2 基线](VDEC-EXT-CODECS.md)

## DRM PRIME 格式与平面

设 `S = ALIGN(width, 256)`，`Y = S × ALIGN(height, 64)`。V4L2 CAPTURE 是一个可导出的 DMA-BUF 对象。DRM PRIME descriptor 的两面或四面都引用该对象。FFmpeg 的 NV21 和 P010 软件格式标签用于选择位深，KMS 按 descriptor 的真实 FourCC、modifier、偏移和步幅取数。

| CAPTURE | DRM FourCC | Modifier | offsets | pitches |
| --- | --- | --- | --- | --- |
| `H8TV` | `NV21` | `0xfe00000000000320` | `0, Y` | `S, S` |
| `H0TV` | `H0TV` | `0xfe00000000000320` | `0, Y, 3Y/2, 7Y/4` | `S, S, S/4, S/4` |

V0 的 `IN_FORMATS` 已声明两组格式，且检查同一 DMA-BUF、四面布局及地址对齐。G0 在 KMS 中为 primary，负责线性 ARGB OSD，`zpos=1`。V0 为 overlay，`zpos=0`。选 `--drm-draw-plane=primary --drm-drmprime-video-plane=overlay`，MPV 依 plane type 选择平面，再由 KMS 校验 framebuffer 与 overlay 的约束。MPV 的 `--vo=drm` 使用软件绘制 VO。DRM PRIME 直扫走 `--vo=gpu --gpu-context=drm --gpu-hwdec-interop=drmprime-overlay`，现有 `drm_prime.c` 将 descriptor 原样用于 PRIME fd 导入及 `drmModeAddFB2WithModifiers`。 [VDP 驱动](src/hi3798mv320-vdp.c) · [MPV DRM 选项](https://github.com/mpv-player/mpv/blob/v0.41.0/DOCS/man/vo.rst) · [MPV PRIME 导入](https://github.com/mpv-player/mpv/blob/v0.41.0/video/out/drm_prime.c)

## 实现与验收路径

- FFmpeg 沿用仓库的 [NV21](scripts/ffmpeg-v4l2request-nv21.patch) 和 [MV320 tiled](scripts/ffmpeg-v4l2request-mv320-tiled.patch) 映射，启用 `h264_v4l2request`、`hevc_v4l2request`、`mpeg2_v4l2request` 和 libdrm。显式传入 `mv320_tiled=1` 选定原生面，该路径拒绝 `hwdownload`。
- MPV overlay 上下文原本注册 `AV_HWDEVICE_TYPE_DRM`。添加 `--drm-v4l2request-mv320-tiled`，由 overlay 注册 FFmpeg `v4l2request` 设备并传递 tiled 选择参数。配合 `--hwdec=v4l2request --hwdec-codecs=h264,hevc,mpeg2video --hwdec-software-fallback=no`。打开 debug 日志可按 KMS FB ID 关联视频 PTS、DRM page-flip 的序列与时间戳。 [MPV 测试补丁](scripts/mpv-v4l2request-drmprime-overlay.patch)
- Main10 验收启用 `--drm-v4l2request-static-g0`。MPV 将一次清零的 ARGB framebuffer 留在 G0 上，并单独提交 V0 翻页，降低 4K60 解码与 G0 渲染之间的带宽竞争。该显式选项关闭本次播放的 OSD 和字幕绘制，常规 G0 渲染用默认路径。另补齐 A/V 两路到 EOF 时等待最后一个已排队 VO 帧的时序。 [MPV 测试补丁](scripts/mpv-v4l2request-drmprime-overlay.patch)
- 内核先用 [已验收的 VDEC-ext RAM 镜像](VDEC-EXT-CODECS.md#实机结果)。该镜像从空目录使用 GCC 14.2.0 构建，零内核警告，媒体驱动内建。当前镜像 SHA256 `1d6be9d318ae3fd04526b7ce519ae1bf9a3359f4e63fed86e6ebed64b295810a`，COM4 回读 CRC32 `399138e9`，BL31 回读 `fa236592`。
- 板端由单个进程独占 COM4 115200 8N1。U-Boot 只设置 RAM 环境变量、通过 TFTP 传输并核对 CRC，再执行 `bootm`。验证 initramfs 和所有节点后，用户态测试包通过隔离网段加载到 `/tmp`。输出保存在 RAM，测试完后从 RAM 取回日志。 [远程构建](scripts/build_mpv_v4l2_drm.sh) · [串口验收](scripts/mpv_v4l2_drm_ram.py)

本轮复用 RAM 和 USB 全媒体基线共有的媒体驱动源码，VDEC、VDP、VENC、JPGD 已与远端全媒体 USB 构建输入逐个对比 SHA256。内核源文件、配置和 DTB 均沿用既有 RAM 镜像。TFTP 启动前检查原 USB 系统的安装情况，将现有 USB 挂载切到只读后复位。切换期间 journald 记录了只读挂载上的写日志失败。用户态测试包和输出均在 RAM，未写入 U 盘、eMMC 或 OTP，未执行 `saveenv`。

## 复现命令

在远端使用仓库现有的 GCC 14.2.0、musl、Linux UAPI 头文件与 Alpine AArch64 SDK。MPV 的 Meson 和 Ninja 分别为 `1.12.1`、`1.13.2`。以下 PowerShell 从项目根目录运行。

```powershell
$root = '/home/hedc/hg680kd-mainline'
$build = "$root/out/arm64-7.3.6-mpv-drm"
$dir = 'artifacts/arm64-7.3.6-mpv-drm'
$key = (Get-ChildItem $env:TEMP -Filter 'hg680kd-ssh-*.dpapi' | Select-Object -First 1).FullName
& ./scripts/remote.ps1 $key run "python3 -m venv $build/tools && $build/tools/bin/pip install meson==1.12.1 ninja==1.13.2"
& ./scripts/remote.ps1 $key put-inputs "$root/scripts" scripts/mpv-v4l2request-drmprime-overlay.patch `
  scripts/ffmpeg-v4l2request-nv21.patch scripts/ffmpeg-v4l2request-mv320-tiled.patch
& ./scripts/remote.ps1 $key script scripts/build_mpv_v4l2_drm.sh $root
New-Item -ItemType Directory -Force $dir | Out-Null
& ./scripts/remote.ps1 $key get "$build/serve/mpv-runtime.tar.gz" "$dir/mpv-runtime.tar.gz"
& ./scripts/remote.ps1 $key get "$build/serve/SHA256SUMS" "$dir/USERSPACE-SHA256SUMS"
Copy-Item artifacts/arm64-7.3.6-vdec-ext/boot.img $dir
Copy-Item artifacts/arm64-7.3.5-vdp/phase3/serve/h264-1080p60.mp4 $dir
Copy-Item artifacts/arm64-7.3.6-usb-display/main10-av.mkv $dir
ffmpeg -y -fflags +genpts -r 30 -i artifacts/arm64-7.3.6-vdec-ext/1080p.m2v `
  -c:v copy -color_primaries bt709 -color_trc bt709 -colorspace bt709 "$dir/mpeg2-1080p30.mkv"
python -u scripts/mpv_v4l2_drm_ram.py $dir
```

RAM chroot 内的 MPV 和 FFmpeg 命令如下。MPV 用 `--no-audio` 播放 H.264 和 MPEG-2，HEVC Main10 使用 AIAO 的 `hw:0,0`。FFmpeg CLI 独立验证 Request 输出为 `drm_prime`。

```sh
MESA_SHADER_CACHE_DISABLE=true mpv --no-config -v --msg-level=vo/gpu=debug,ao/alsa=v \
  --vo=gpu --gpu-api=opengl --gpu-context=drm --drm-device=/dev/dri/card0 \
  --gpu-hwdec-interop=drmprime-overlay --drm-draw-plane=primary \
  --drm-drmprime-video-plane=overlay --drm-v4l2request-mv320-tiled \
  --hwdec=v4l2request --hwdec-codecs=h264,hevc,mpeg2video --hwdec-software-fallback=no \
  --osd-level=0 --swapchain-depth=1 --video-sync=display-resample \
  --term-status-msg='MPV_STATUS vo_drop=${frame-drop-count} dec_drop=${decoder-frame-drop-count}' \
  --drm-v4l2request-static-g0 --framedrop=no --ao=alsa --audio-device=alsa/hw:0,0 \
  --audio-format=s16 --audio-samplerate=48000 \
  --audio-channels=stereo /tmp/main10-av.mkv

for clip in h264-1080p60.mp4 mpeg2-1080p30.mkv main10-av.mkv; do
  ffmpeg -nostdin -v verbose -init_hw_device v4l2request=mv320:,mv320_tiled=1 \
    -hwaccel v4l2request -hwaccel_device mv320 -hwaccel_output_format drm_prime \
    -i "/tmp/$clip" -an -c:v wrapped_avframe -f null -
done
```

## 实机证据和计数口径

`mpv --hwdec=help` 枚举到 `h264-v4l2request`、`hevc-v4l2request`、`mpeg2video-v4l2request`。FFmpeg CLI 对 H.264、MPEG-2 和 HEVC 分别输出 1080、120 和 1080 帧 `wrapped_avframe` / `drm_prime`，日志说明选中了 `/dev/video0` 对应的 V4L2 media driver。MPV 日志同时记录 `Using hardware decoding (v4l2request)`、overlay 的真实 FourCC/Modifier 和 DRM 翻页时的 V0 FB ID。内核对三种码流均记录 `detile=0`。KMS 的 V0 framebuffer 对象显示 `imported=yes`，G0 使用线性 ARGB。 [FFmpeg 与 COM4 验收摘录](logs/540-mpv-v4l2-drm-acceptance.txt)

上屏计数逐条按 KMS FB ID 的最新提交记录关联画面 PTS，并对重复呈现的同一 PTS 去重。FPS 用首尾 DRM 翻页时间戳与期间上屏帧数计算。漏 vblank 由相邻上屏时间之差与片源 PTS 所需刷新次数比较，剔除 30 fps 在 60 Hz 下的正常间隔。MSC 序号用同一时间戳复核。早期诊断将重复使用的 FB ID 一次性关联到最后一次 PTS，误报了 H.264 的漏 vblank，最终按日志顺序关联。CPU 取视频启动前和退出后的 `/proc/stat`，报告各核 busy 时间比例。XRUN 计数取 ALSA 恢复日志的 `SND_PCM_STATE_XRUN`。

4K60 动态 G0 的首轮用标准 MPV EGL 交换链，硬解 1080 帧并上屏 923 帧，实测 51.128 FPS。交换链深度增加后稳态接近 60 FPS，队列启停仍漏首尾。采用静态透明 G0、每帧单独提交 V0 并等待翻页，随后补上 A/V EOF 退场等待。KMS 逻辑模式为 1920×1080@60，固件 HD0 为 3840×2160，Main10 原生的 4 个 tiled 面送入 V0。HDMI 采集卡按 1080p30 采样，这里用来验证可见画面与 PCM 信号。

## 最终 COM4 验收

| MPV 片源 | V0 上屏 | 实际 FPS | 掉帧 VO / 解码 | 漏 vblank | XRUN | CPU0 / 1 / 2 / 3 | VDH INTID 169 增量 |
| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| H.264 1920×1080p60 | 1080/1080 | 59.944 | 0 / 0 | 1 | 视频单独测试 | 15.76% / 14.32% / 14.21% / 12.49% | +1080 |
| MPEG-2 1920×1080p30 I/P/B | 120/120 | 30.127 | 0 / 0 | 0 | 视频单独测试 | 19.65% / 6.17% / 14.38% / 9.71% | +120 |
| HEVC Main10 3840×2160p60 + AIAO | 1080/1080 | 60.000 | 0 / 0 | 0 | 0 | 18.16% / 17.26% / 15.11% / 16.21% | +1080 |

Main10 使用 `alsa/hw:0,0`，48 kHz、立体声、S16。INTID 116 增加 719，采集的 PCM 在播放段 RMS 为 −24.24 dBFS。MPV 在文件结束后的 ALSA 收尾记录了一次 `drain timeout`，检查时 PCM 已关闭。 [MPV Main10 验收摘录](logs/540-mpv-v4l2-drm-acceptance.txt)

三轮退出后 DMA-BUF 均为 0 个、0 B。`CmaFree` 依次为 `380056 → 380276`、`380308 → 380292`、`380292 → 380184` KiB。第一轮结束后一度比基线多占 8 个 CMA 页，全部回归结束后 `reserved/used` 恢复到 3209 页，后续空闲量回到 380056 KiB。INTID 181 的累计值从启动到末次抽查保持为 0。VDP underflow 计数和寄存器均为 0。各轮 V0 退出后关闭，G0 恢复原 `fbcon` FB 48，HDMI 采集确认控制台可见。RAM 环境验收的是 fbcon。 [结构化结果](logs/540-mpv-v4l2-drm-result.json)

| 回归 | 结果 |
| --- | --- |
| 双 PP Lima | PP0、PP1 各 16/16，双 PP 同时跑 32/32，逐作业匹配 4096/4096 像素。 |
| JPGD `/dev/video1` | 1920×1080 JPEG 输出 NV12，INTID 129 +1。 |
| VENC `/dev/video2` | 1920×1080 NV12 编码 12 帧，INTID 133 +12，主机 FFmpeg `-xerror` 严格解码通过。 |
| 原 `vdp-player` | 4K60 Main10 + G0 OSD 上屏 1080/1080，60.000 FPS，零掉帧、零漏 vblank。AIAO 输出 864000 帧 PCM，XRUN 为 0。 |

复用的 `boot.img`、用户态包与构建日志位于 `artifacts/arm64-7.3.6-mpv-drm/`。用户态包 SHA256 `029be63ae3e11e6b53f3f52b20cf0a4b5833f82eac4372b96d0652806b17f3fc`。MPV 用 GCC 14.2.0 构建，最终编译日志零警告。FFmpeg 同用 GCC 14.2.0，日志有 4 条来自 `rawdec.c` 和 `vlc.c` 的警告。 [镜像和用户态包校验](logs/540-mpv-v4l2-drm-acceptance.txt)

工作区的 `artifacts/arm64-7.3.6-mpv-drm/` 另存 `mpv-com4.log`、逐片 MPV 与 FFmpeg 日志、`main10-hdmi-capture.mkv`、`main10-mpv-hdmi.png` 和 `console-after.png`。
