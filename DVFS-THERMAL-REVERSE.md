# HG680KD 原厂温控与 DVFS 逆向

对象为同一台板型 4 的 HG680KD，Hi3798MV320、2 GiB LPDDR4 和 Mali-450。原厂 `factory.dtb` SHA256 为 `e289000306072f9fd2e140dc5a837fb646b9c803dae15c2b4d0b99c32cf63517`，与历史从 eMMC 只读提取的 `dtbo` 内 FDT 一致。原厂 ARM32 内核映像 SHA256 为 `a47592ad5a842035f0bd6e41fdce71cb89c2238aff57e2b333d60213af176724`，下文函数地址为恢复的 kallsyms 虚拟地址，以 `_text=0xc0008000` 换算文件偏移。原厂 DT 由服务器 `inputs/factory.dtb` 反编译，指令用本机 Capstone 5.0.6 解码。从既有只读 eMMC 副本用 `debugfs` 检查了 system/vendor 模块目录，vendor 的八个模块均用于无线或蓝牙，TSENSOR、DVFS、PWM regulator 与 Mali 实现由原厂内核映像中的符号和调用关系确认。[原厂 FDT 来源](WIFI-REVERSE.md) · [原厂内核来源](HDMI-REVERSE.md) · [原厂 DT 和分区证据](logs/346-dvfs-factory-evidence.txt)

## TSENSOR

- PMC 基址 `0xf8a23000`，CPU TSENSOR 窗口 `0xf8a23028–0xf8a23043`。`tsensor_probe` (`0xc052d514`) 和 `tsensor_resume` (`0xc052d464`) 均向 `0xf8a23028` 写入原厂控制值 `0x72`（bit 1、4、5、6 置位）。持续采样由实机温度序列复核，单独各位的功能尚无本 SoC 的寄存器定义佐证。
- `ipa_get_sensor_value` (`0xc052d330`) 读 `0xf8a23030/34/38/3c`。每字从 `[9:0]` 与 `[25:16]` 各取一个 10-bit ADC 样本，八样本求和后右移三位得到 `adc`。正常 ADC 范围内，`t_mC = 1000 * ((165 * (adc - 132)) / 808 - 40 + trim_C)`。除法是整数运算，`get_temp_value` (`0xc052db90`) 乘 `1000` 前还检查有校准的读数在 `−45…130 °C` 内。主线把尚未填满的零值采样返回为 `-EAGAIN`。
- 原厂读 `0xf8ab0000 + (0x1db & ~3)` 的 `[31:24]`。`trim_C = trunc_toward_zero((bit7 ? -1 : 1) * ((byte & 0x7f) + 2) * 25 / 100)`。范围为 −32…+32 °C。本次实机与主线代码均未访问 OTP 寄存器，当前 sysfs 温度为未校准读数。
- 原厂 `soc_thermal` 使用 `soc_max`（该 DT 只有 CPU TSENSOR），常态轮询 200 ms、被动轮询 50 ms，100 °C 被动控制起点、115 °C CPU/GPU 冷却目标、118 °C hot、120 °C critical，迟滞均 1 °C。独立 DVFS 温度线程另配置 105/95 °C 老化限频与 120 °C standby。主线镜像保留轮询和迟滞，用较保守的 68/83/86/88 °C 覆盖未知正向 Trim，`step_wise` 从第一个被动点同时限制 CPU 和 GPU。critical 关机未通过实机升温触发。[U-Boot TSENSOR 初始寄存器](logs/337-dvfs-uboot-registers.txt) · [Linux thermal 接口](https://docs.kernel.org/driver-api/thermal/sysfs-api.html)

## PWM 供电

| 供电轨 | 原厂逻辑 | 原厂约束 | 本机 U-Boot 初始值 |
| --- | --- | --- | --- |
| CPU | `regulator_2`，PMC `+0x18` | 750,000–1,200,000 µV | `0x004b00dd`，驱动公式回读约 1,048 mV |
| 核心 `dvdd` | `regulator_1`，PMC `+0x1c`，原厂 `hi,dvfs` 的 `core_regulator_name` | 750,000–1,200,000 µV | `0x009500dd`，驱动公式回读约 897 mV |
| Mali-450 | `mali-supply` 指向上述 `dvdd`，别名 `vdd-gpu` 的 `reg` 也映射 `+0x1c` | `vdd-gpu` 虚拟约束 720,000–1,185,000 µV | 与 `dvdd` 共用。`+0x20` 实读 0 |

原厂 `hi_regulator_set_voltage` (`0xc052b2bc`) 对 CPU 和核心保留 `[15:0]` 的周期字段，写 `[31:16]` 的 duty 计数 `d = floor(221 * (1200 - V_mV) / 450)`，`hi_regulator_get_voltage` (`0xc052b3e0`) 回读 `V_mV = 1200 - floor(450 * d / 221)`。每个计数对应约 2036 µV，电压随 `d` 增大而下降。周期字段实读 221，PWM 输入时钟与波形周期尚无测量。CPU 调压走直接写入，原厂 `dvdd` 另有 5 mV 步进与 1000 µs 等待的降压路径。

旧 `vdd-gpu` 包装 `hi_gpu_regulator_set_voltage` (`0xc052b0fc`) 走另一套标度。导出 helper (`0xc052b048`) 对两个需求槽取最大值、用 1185 mV 封顶，写 `((2 * floor((1270 - V_mV) / 5) + 1) << 16) | 221` 到同一核心 PWM。`mali_devfreq_target` (`0xc098c388`) 正常运行经 DT 的 `mali-supply` 使用 `dvdd`。两条软件路径的标度、约束不同，不能叠加两个可写 regulator。主线只注册 CPU regulator，固定其硬件上下限和已验证 PWM 周期值，GPU Devfreq 沿用 U-Boot 初始化的 `dvdd` 电压。表内电压来自寄存器换算，尚无示波器或万用表读数。[现场 CPU 与核心 PWM](logs/337-dvfs-uboot-registers.txt) · [GPU HPM 与 PMC `+0x20`](logs/338a-dvfs-uboot-gpu-hpm.txt)

## CPU 时钟和原厂 OPP

CRG 基址 `0xf8a22000`。`clk_recalc_rate_cpu` (`0xc0523abc`) 读 `+0x48[2:0]`，选择值 0 为 APLL。`+0x00[26:24]` / `[30:28]` 是两个 postdiv，`[23:0]` 为小数，`+0x04[17:12]` 是 refdiv，`[11:0]` 是 fbdiv，bit 25 为整数模式。按 `24 MHz * (fbdiv + frac / 2^24) / (refdiv * postdiv1 * postdiv2)` 回读，启动值 `0x122aaaaa/0x08001068` 给出约 1250 MHz。原厂 `recalc_rate` 只用整数并将对应 1.25 GHz 区间报告为 1.5 GHz，原厂 1.5 GHz 设频入口也转换到 1.25 GHz。

`clk_set_rate_cpu` (`0xc0523b70`) 首先查 `+0x150` bit 0 的 APLL lock。200 MHz 档设 `+0x48[4:3]/[23:22]` 为 1、延时，再把 `[2:0]/[21:19]` 设 4，另写 `+0x4b0[22:20]=7` 和 `+0x48[8:6]=5`。从 200 MHz 升频时，原厂保持这个非 APLL 源直到重锁。原厂在 APLL 档之间直接使用硬件调整器，没有先切走。调整器使用 `+0x1a4` 的 bit 16/20 和 `[3:0]`、`+0x1a8` 的 `[9:8]` 与 bit 4/0、`+0x1ac/+0x1b0=0/0xaaaaa`，装载新 fbdiv/frac 后启动并轮询 `+0x1b4` bit 0。完成后回到 APLL，再按 `g_freq_match_info` 设置辅助字段。主线对每次 APLL 改频先切入已确认的 200 MHz 路径，两种锁状态均确认后再切回，超时保持 200 MHz。

原厂 `operating-points-v2` 下表各档均填 `1,000,000 µV` 和 `300,000 ns`，包括两个禁用节点。运行时由 `dvfs_init_corner_volt` (`0xc0962bb8`) 取 `0xf80000b4[31:16]` 的 CPU HPM，实际 `H=293`。`init_corner_volt` (`0xc0962988`) 及 `dvfs_get_volt_for_new_freq` (`0xc0961da8`) 求 `V0 = intercept − floor(slope * H / 1000)`，再加 `otp_added` 并夹到 `min…upper`，最后叠加温度/老化项并受 `max` 限制。OTP 补偿启用时，每档指定半字节乘 15 mV。补偿未启用时改用 `fallback`。引导日志中的 `cpu_otp_comp=45 mV` 是辅助代码输出，无法代表原厂内核每档独立的半字节。[时钟与 HPM 现场值](logs/337-dvfs-uboot-registers.txt) · [原厂启动校准日志](logs/344-dvfs-reboot-to-uboot.txt)

| DT MHz | 状态 | slope | intercept (mV) | fallback (mV) | min/upper/max (mV) | 原厂 CRG 表辅助字段 `[8:6]` / `[10:9]` / `+0x4b0` 除数 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 200 | 启用 | 1285 | 1074 | 55 | 810/1100/1200 | 5 / — / 8 |
| 400 | 启用 | 1285 | 1074 | 55 | 810/1100/1200 | 5 / — / 7 |
| 600 | 启用 | 1382 | 1175 | 40 | 810/1100/1200 | 7 / 3 / — |
| 800 | 启用 | 1434 | 1258 | 40 | 810/1100/1200 | 7 / 2 / — |
| 1000 | 启用 | 1564 | 1379 | 40 | 810/1100/1200 | 7 / 0 / — |
| 1250 | 禁用 | 1747 | 1515 | 40 | 810/1100/1200 | 2 / — / — |
| 1300 | 禁用 | 1808 | 1550 | 40 | 810/1100/1200 | 6 / — / — |
| 1500 | 最高策略档，约 1250 MHz 实频 | 1747 | 1515 | 40 | 810/1100/1200 | 1 / — / —，入口重映射到 1250 |
| 1650 | 表中启用，受 1500 MHz 策略上限限制 | 0 | 1150 | 0 | 810/1150/1200 | 1 / — / — |
| 2000 | 表中启用，受策略上限限制 | 0 | 1150 | 0 | 810/1150/1200 | 查表无条目 |

该镜像使用 200–1000 MHz 和 1.05/1.10 V 两档保守 OPP。最低三档要求 `H ≥ 256`，600 MHz 即使加满 225 mV 校准仍不超过 1047 mV。末两档采用原厂常态上限 1100 mV。实际 PWM 格点为约 1051/1100 mV。CPU regulator 保留 `750000/1200000` 原厂边界，拒绝未知周期和低于当前实频所需的向下转换。Linux `cpufreq-dt` 对四核使用共享 OPP，支持 schedutil、performance、powersave 和 userspace。主线开机读取真实 1250 MHz，再选表内 1000 MHz。[OPP 机制](https://docs.kernel.org/power/opp.html) · [CCF 接口](https://docs.kernel.org/driver-api/clk.html)

## Mali-450 时钟和原厂 OPP

`hi_gpu_clk_set_rate` (`0xc0523378`) 用单次写入切 `CRG+0x124` 的选择值，无 APLL 调整或额外门控步骤。完整映射如下，具体物理 PLL 父源和内部除数未由原厂代码公开。原厂 `mali_devfreq_target` 升频时先升压、降频时后降压。`mali_vmodel_get_volt` (`0xc09a5c80`) 计算 `max(810, min(1200, min(1050, C − floor(R * H / 1000) + otp) + temp_gain))` mV。`mali_hpm_init` 从 `0xf80000f8[15:0]` 读到 275，五档 `operating-points-tt` 的第二列是各自 kHz 数值，实际电压由模型替换。

| MHz | `+0x124[2:0]` | 原厂 DT OPP | `C` / `R` | 低温/高温补偿 (mV) | OTP 未启用回退时常温/低温 (mV) |
| ---: | ---: | ---: | --- | --- | --- |
| 200 | 7 | 200000 | 1145 / 1152 | 40 / 0 | 869 / 909 |
| 300 | 4 | 300000 | 1145 / 1152 | 40 / 0 | 869 / 909 |
| 400 | 1 | 400000 | 1172 / 1225 | 50 / 0 | 876 / 926 |
| 460 | 6 | — | — | — | — |
| 540 | 0 | 540000（默认） | 1172 / 1225 | 50 / 0 | 876 / 926 |
| 675 | 5 | — | — | — | — |
| 712 | 2 | 712000（上限） | 1363 / 1655 | 30 / 0 | 948 / 978 |

原厂 GPU 在 35/40 °C 和 40/45 °C 设两组补偿迟滞。未启用 OTP 时上表以 `vmodel-otp-volt=40 mV` 计算。启用时 `mali_otp_init` (`0xc09a5fdc`) 使用每档半字节乘 15 mV，200/300 共享一个字段，400/540 共享一个字段，712 独立。主线仅启用原厂电压模型相同的 200/300 MHz，在本台、当前室温条件下使用同一 U-Boot `dvdd`，其余档位与 GPU 独立调压未开放。[GPU 现场 HPM](logs/338a-dvfs-uboot-gpu-hpm.txt)

## 实机验证记录

- **RAM 启动与供电轨**：[360-dvfs-supplies-boot.txt](logs/360-dvfs-supplies-boot.txt) · [361-dvfs-info.txt](logs/361-dvfs-info.txt)（`cpu` 轨 1051358 / 1100227 µV，`dvdd` 只读回读 896607 µV，`soc-thermal` trips 68/83/86/88 °C）
- **CPUFreq 调频调压与算力标定**：[362-dvfs-cpufreq-steps.txt](logs/362-dvfs-cpufreq-steps.txt) · [363-dvfs-cpufreq-bench.txt](logs/363-dvfs-cpufreq-bench.txt)（200/400/600/800/1000 MHz，`sha256sum` 32 MiB 耗时 5.81 s / 1.92 s / 1.15 s）
- **四核满载升温与停载回落**：[364-dvfs-thermal-stress.txt](logs/364-dvfs-thermal-stress.txt)（56 °C → 67 °C → 59 °C，`schedutil` 自动升降频）
- **GPU Devfreq 与 Lima 渲染**：[365-dvfs-gpu-devfreq.txt](logs/365-dvfs-gpu-devfreq.txt)（200/300 MHz 均通过 `lima-test` 4096/4096 像素匹配）
- **全项外设回归**：[366-dvfs-full-regression.txt](logs/366-dvfs-full-regression.txt)（四核 SMP、`MemTotal = 2,023,512 kB`、HDMI 1080p、RTL8761BTV `hci0`、GMAC 0% 丢包、USB U 盘、eMMC `ro=1`）
