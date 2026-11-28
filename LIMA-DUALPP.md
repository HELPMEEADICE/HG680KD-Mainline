# HG680KD Mali-450 双 PP 验收

Linux `7.3.6-hg680kd-arm64-lima-ram` 使用 Linux 7.3.6 USB 显示基线及内置的 AIAO RAM initramfs。诊断组共用一份内核，分别使用单 PP 和双 PP DTB。两种镜像都经 COM4 115200 8N1 独占控制，由 U-Boot 从 TFTP 装入 RAM 并回读 CRC32。最终镜像同时启用 PP0、PP1、PPMMU0 和 PPMMU1，禁用 eMMC 控制器和 USB 大容量存储。

## 中断定位

原厂 `factory.dtb` 的 SHA256 为 `e289000306072f9fd2e140dc5a837fb646b9c803dae15c2b4d0b99c32cf63517`。其 `IRQGP`、`IRQGPMMU`、`IRQPP0`、`IRQPPMMU0`、`IRQPP1`、`IRQPPMMU1`、`IRQPMU` 和 `IRQPP` 都是 `<0 0x5e 0x04>`。Lima 的广播 IRQ 使用 DT 中的 `pp` 名称，注册的 handler 名称为 `pp_bcast`。GIC SPI 94 对应 INTID 126，为高电平触发。单 PP 拓扑没有注册 `pp1` 或 `ppmmu1`。

PP 各列按 `STATUS / INT_RAWSTAT / INT_MASK / INT_STATUS` 排列，数值为十六进制。BCAST 列按 `BROADCAST_MASK / INTERRUPT_MASK` 排列。`GIC` 为 GICD 的 enable、pending、active 和 ICFGR 触发类型字段。MMIO 基址为 `0xf1000000`，前三项在第 3 组寄存器的 bit 30，ICFGR 字段位于 `ICFGR7[29:28]`。 [Arm GICv3/v4 软件指南](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/GICv3_v4_overview.pdf?revision=65f91645-cd52-4795-952b-f01095ff5ef8)

| DT 拓扑 / 任务 PP 数 | 采样点 | PP0 | PP1 | PPBCAST | BCAST | GIC | 结果与 IRQ 增量 |
|---|---|---|---|---|---|---|---|
| 单 PP / 1 | 启动前 | `0/0/f19/0` | `0/1000/fff/0` | `0/0/f19/0` | `10001/1` | `1/0/0/0` | |
| 单 PP / 1 | PP IRQ 入口 | `40/1/f19/1` | `0/1000/fff/0` | `40/1/f19/1` | `10001/1` | `1/1/1/0` | `done=1`，4096/4096，INTID 126 `+2` |
| 双 PP / 1 | 启动前 | `0/0/f19/0` | `0/0/f19/0` | `0/0/f19/0` | `30001/3` | `1/0/0/0` | |
| 双 PP / 1 | `timedout_job` | `40/1/f19/1` | `0/0/f19/0` | `40/1/f19/1` | `30001/3` | `1/0/0/0` | `task=1, done=0`，4096/4096，INTID 126 `+1` |
| 双 PP / 2 | 启动前 | `0/0/f19/0` | `0/0/f19/0` | `0/0/f19/0` | `30003/3` | `1/0/0/0` | |
| 双 PP / 2 | PP IRQ 入口 | `40/1/f19/1` | `40/1/f19/1` | `40/1/f19/1` | `30003/3` | `1/1/1/0` | `done=3`，4096/4096，INTID 126 `+2` |

`/proc/interrupts` 将 `gp`、`gpmmu`、`pp0`、`ppmmu0`、`pp1`、`ppmmu1` 和 `pp_bcast` 列在同一个 INTID 126 上，每 CPU 只有该共享线的计数。三组均先出现 GP 完成，PP handler 随共享线进入，此时各 PP 的 `INT_STATUS=0`，`task=0`。成功组随后出现 PP 完成中断并调用 `lima_sched_pipe_task_done()`；超时组只有 GP 那一次，`timedout_job` 仍读到 PP0 和 PPBCAST 的帧完成位及三个 MMU 的零中断状态。用户等待在 2 秒时返回超时，内核在 10 秒时进入 `timedout_job`。 [单 PP](logs/524-lima-single-pp-diag.txt) · [双 PP 拓扑上的单 PP 任务](logs/525-lima-dual-topology-one-pp-diag.txt) · [双 PP 任务](logs/526-lima-dual-topology-two-pp-diag.txt)

根因是 BCAST 初始化按在线拓扑将 `INTERRUPT_MASK` 设为 `0x3`，任务提交只更新 `BROADCAST_MASK` 的低位。双 PP 拓扑上的单 PP 任务呈现 `0x30001/0x3`。PP0 的 EOF 已进入本地与广播状态寄存器，共享 SPI 没有置 pending，广播 handler 无法递减任务计数和通知 `drm_sched`。

## 修复与复测

补丁以本次提交选中的物理 PP 位同时设置广播目标和广播中断掩码，保留 MMU 广播位。切换核数时，先按上一任务的广播掩码等待异步软复位，再编程新任务。超时恢复使用真正的 BCAST 寄存器区屏蔽和恢复广播配置，并清除已由硬复位替代的异步复位状态。 [补丁](patches/0018-drm-lima-match-pp-broadcast-to-job.patch)

临时插桩复测中，双 PP 在线且提交单 PP 任务时读到 `0x30001/0x1`。PP1 状态仍为 `0/0/f19/0`，PP0 与 PPBCAST 均读到 `40/1/f19/1`，GICD 在 PP IRQ 入口为 `1/1/1/0`，`pp_bcast` 通知 `drm_sched` 完成。最终镜像已去掉这些插桩。 [修复后单 PP 寄存器](logs/531-lima-dualpp-fixed-one-pp-diag.txt)

- GCC 14.2.0 编译零警告。最终 `boot.img` 大小为 19,761,224 字节，SHA256 为 `110e374d0fa008c59b0175494a86ba91713c3a334e981264f1bc0f619c486290`。U-Boot 在 `0x01fdffc0` 对整个镜像回读 CRC32 `6d540fd9`，与主机一致。BL31 RAM 副本回读 CRC32 为 `fa236592`。 [构建日志](logs/529-lima-dualpp-build.txt) · [最终 RAM 启动](logs/527-lima-dualpp-ram-acceptance.txt)
- 双 PP 在线时，固定单 PP 与固定双 PP 各 16 个任务，单/双 PP 交替 32 个任务。64 个任务均正常完成并匹配 4096/4096 像素，INTID 126 分别增加 32、32、64，Lima 无错误日志。 [最小场景](logs/527-lima-dualpp-ram-acceptance.txt)
- 最终镜像从 `/dev/dri/renderD128` 使用 `Mali450` 和 Mesa 25.2.7 跑 OpenGL ES 2.0 离屏纹理绘制，每轮 60 帧、分辨率 640×480。三轮像素回读均通过，INTID 126 每轮增加 180。相同修复的临时插桩镜像另跑一轮，60 个 PP 完成事件同时读到 PP0、PP1 各自的 EOF 位。 [GLES 测试源码](src/lima-gles-test.c) · [最终镜像 Mesa 日志](logs/530-lima-dualpp-ram-mesa.txt) · [逐核 EOF 证据](logs/532-lima-dualpp-mesa-core-diag.txt)
- `/dev/video1` 成功解码 1080p JPEG，INTID 129 增加 1。`/dev/video0`、`/dev/dri/card0` 和 `/dev/snd/pcmC0D0p` 完成 2160p Main10 解码、HDMI 上屏及 PCM 播放。解码和显示均为 1080 帧，60 FPS，零丢帧、零漏 vblank。AIAO 播放 864,000 帧 48 kHz PCM，零 XRUN。VDEC、VDP、AIAO 的 IRQ 分别增加 1080、1090、1800，SMMU INTID 181 为 0。 [媒体回归](logs/528-lima-dualpp-ram-media.txt)

## USB 桌面负载

USB 桌面负载进一步暴露了分批完成的情况。Mesa 提交的双 PP 任务在超时时为 `task=1, done=0x2`，PP1 已确认完成，PP0 和 PPBCAST 都有 EOF，BCAST 掩码仍为 `0x30003/0x3`，GICD 的 pending 和 active 为 0。补丁 0019 在共享 IRQ 部分完成后将中断掩码收窄到剩余的 PP，保留本次任务的广播执行位和 MMU 位。上面的 RAM 验收与镜像校验值记录的是补丁 0018 阶段。 [补丁 0019](patches/0019-drm-lima-rearm-shared-pp-broadcast-irq.patch) · [USB 部署](UBOOT-USB-BOOT.md#linux-736-dual-pp-usb-deployment)

## 远程重建

使用 `scripts/remote.ps1` 将 Lima 补丁 0018 和 0019、`src/lima-test.c` 和 `scripts/pack_arm64.py` 上传至服务器上的 `out/lima-dualpp/input/`。构建默认使用 `out/arm64-7.3.6-usb-display` 的源码与配置，以及已验收的 `out/release-arm64-7.3.6-aiao/initramfs.cpio`。

```powershell
$hgKey = (Get-ChildItem -LiteralPath $env:TEMP -Filter 'hg680kd-ssh-*.dpapi' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
& scripts/remote.ps1 -PasswordFile $hgKey script scripts/build_lima_dualpp_736.sh /home/hedc/hg680kd-mainline
```

输出在远端 `out/lima-dualpp/final/`。补丁 0018 阶段的实测镜像保存在本地 `artifacts/arm64-7.3.6-lima-dualpp/boot.img`，附 `SHA256SUMS` 和串口测试脚本 `lima-run.py`。板子在逐核诊断结束后重新经 TFTP 启动该镜像，第二轮 64 个最小场景任务也全部通过。纯 RAM 诊断与验收期间，U 盘 `/boot/boot.img` 覆盖、eMMC 刷写、OTP 操作及 `saveenv` 执行次数均为 0。
