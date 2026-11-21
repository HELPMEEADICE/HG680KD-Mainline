# 烽火 HG680KD（Hi3798MV320）原厂引导链 ARM64（AArch64）交接分析与实机验证

## 1. 结论摘要

经对从实机只读 `/dev/mmcblk0` 提取的原厂 `fastboot`（2 MiB，SHA256 `c0889ada28f6b8ccab76b983394a0470028efd8324412eb8ce102c64480a8265`）与 `atf`（2 MiB，SHA256 `72b2f15f68537c899222d691f8fa6ad8fa18055a8b77792fdc224bed1b9d56bf`）反汇编分析，并经 COM4 实机纯 RAM 引导验证：

1. **原厂 `fastboot`（U-Boot 2022.07）**：32 位编译（链接基址 `0x00c00000`，重定位地址 `0x00b0c000`），运行在 **AArch32 Secure SVC / Monitor（EL3）**。
2. **原厂 `atf`（BL31 `lts-v2.8.6(release):98d5f24`）**：装载于 `0x08020000`，运行在 **AArch64 EL3**。
3. **`bootm` 原生支持 `IH_ARCH_ARM64`（`#22`）**：无需刷写 eMMC、无需修改 U-Boot/BL31、无需触碰 `saveenv`/OTP，只需提供标准 Legacy `uImage`（`ih_arch = 22`）并附带 32 字节 AArch64 入口 shim 设置 `x0 = DTB`，即可纯 RAM 切入主线 **Linux 6.18.64 AArch64（EL1h）**，四核 Cortex-A55（`0x000–0x300`）全在线，进入 AArch64 BusyBox 1.37.0 shell（`uname -a` 输出 `aarch64`）。

---

## 2. 原厂二进制反汇编证据

### 2.1 U-Boot 执行级别确认（`switchns` @ `0x00c34af0`）

`fastboot.bin` 命令表 `0x00ce3fd0` 将 `switchns` 指向 `0x00c34af0`：

```arm
00c34b04: f1020016  cps  #22                     @ 切入 Monitor 模式（EL3 AArch32）
00c34b0c: ee114f11  mrc  p15, 0, r4, c1, c1, 0   @ 读 SCR
00c34b10: e3a01001  mov  r1, #1
00c34b14: ee011f11  mcr  p15, 0, r1, c1, c1, 0   @ 写 SCR.NS = 1
00c34b18: f1020013  cps  #19                     @ 返回 SVC 模式
```

证明 U-Boot 处于 **AArch32 EL3（Secure SVC/Monitor）**，拥有读写 `SCR` 与 `RMR` 的最高权限；`switchns` 本身仅用于将 AArch32 切为 Non-Secure，不负责切换 64 位。

### 2.2 `bootm` 对 `IH_ARCH_ARM64` 的分发（`boot_jump_linux` @ `0x00c1e864`）

`boot_jump_linux` 先调用 `0x00c1b730` 读取 `*(0xf8ab01cc) & 1`（TEE 使能标志，本机实测为 `0`），随后检查 Legacy `uImage` 头的 `ih_arch` 字段（`[r5, #33]`）：

```arm
00c1e8f4: e5d53021  ldrb r3, [r5, #33]           @ hdr->ih_arch
00c1e8f8: e285a004  add  sl, r5, #4              @ sl = &images->legacy_hdr_os_copy
00c1e8fc: e3530002  cmp  r3, #2                  @ IH_ARCH_ARM
00c1e900: 0a000051  beq  0xc1ea4c                @ "Start ATF with aarch32 kernel...", start_atf(32, ...)
00c1e904: e3530016  cmp  r3, #22                 @ IH_ARCH_ARM64
00c1e908: 0a000003  beq  0xc1e91c                @ "Start ATF with aarch64 kernel...", start_atf(64, ...)
00c1e90c: e59f01e4  ldr  r0, [pc, #484]          @ "ARCH type not support!!!"
...
00c1e91c: e59f01d8  ldr  r0, [pc, #472]          @ "Start ATF with aarch64 kernel..."
00c1e920: eb00f871  bl   printf
00c1e92c: e1a0100a  mov  r1, sl                  @ hdr
00c1e930: e1a03008  mov  r3, r8                  @ 0
00c1e934: e1a02008  mov  r2, r8                  @ 0 (fdt = 0)
00c1e938: e3a00040  mov  r0, #64                 @ width = 64
00c1e93c: ebffff2f  bl   0xc1e600                @ start_atf(64, hdr, 0, 0, 0, 0)
```

### 2.3 U-Boot 构造 `bl33_ep` 并通过 `RMR` Warm Reset 切入 AArch64 EL3（`start_atf` @ `0x00c1e600`）

当 `r0 == 64` 时，进入 `0x00c1e7d8` 分支，在栈上填充 `bl31_params_t`（48 B）与 `bl33_ep`（`entry_point_info_t`，88 B）：

- `bl33_ep->pc`（`[sp, #144]`）= `be32_to_cpu(hdr->ih_ep)`（uImage 入口地址，高 32 位清零）
- `bl33_ep->spsr`（`[sp, #152]`）= **`0x3C5`**（`SPSR_64(MODE_EL1, MODE_SP_ELX, DISABLE_ALL_EXCEPTIONS)`，即 **AArch64 EL1h，DAIF 全屏蔽**）
- `bl33_ep->args.arg0`（`[sp, #160]`）= `r2`（`0`，因该 U-Boot 未启用 `bootm` FDT 传参）
- `bl33_ep->args.arg1`（`[sp, #168]`）= `be32_to_cpu(hdr->ih_ep)`
- `bl33_ep->args.arg2`（`[sp, #176]`）= `be32_to_cpu(hdr->ih_size)`

随后在 `0x00c1e6ec`–`0x00c1e750` 完成 AArch32 EL3 → AArch64 EL3 切换：

```arm
00c1e6fc: e5832034  str  r2, [r3, #52]           @ *(0xf8a80034) = 0x08020000 (RVBAR_EL3)
00c1e708: e382200f  orr  r2, r2, #15
00c1e714: e5832030  str  r2, [r3, #48]           @ *(0xf8a80030) |= 0xf (四核复位为 AArch64)
00c1e724: e1cc40f0  strd r4, [ip]                @ *(uint64_t *)0x0 = &bl31_params
00c1e72c: e1cc20f8  strd r2, [ip, #8]            @ *(uint64_t *)0x8 = 0
00c1e730: eb00f8ed  bl   printf                  @ "Start Warm Reseting"
00c1e734: ebfff80c  bl   cleanup_before_linux
00c1e740: ee1c3f50  mrc  p15, 0, r3, c12, c0, 2  @ 读 RMR
00c1e744: e3833003  orr  r3, r3, #3              @ AA64=1 | RR=1
00c1e748: ee0c3f50  mcr  p15, 0, r3, c12, c0, 2  @ 写 RMR 触发 AArch64 EL3 Warm Reset
00c1e74c: f57ff06f  isb  sy
00c1e750: e320f003  wfi
```

### 2.4 原厂 BL31（`atf.bin` @ `0x08020000`）接管与 `eret` 交接

- `0x08020000`–`0x0802000c`：`ldr x20, [x0]` 从物理地址 `0x0` 读取 U-Boot 写入的 `&bl31_params`。
- `bl31_early_platform_setup2`（`0x08023a8c`）：从 `bl31_params->bl33_ep_info` 拷贝 88 字节至 `0x08035bd0`，并设置 `NON_SECURE` 属性。
- `0x08023b30`–`0x08023b3c`：初始化架构定时器（`cntvoff_el2 = 0`、`cnthctl_el2 = 3`、`cntkctl_el1 = 3`）。
- `bl31_prepare_next_image_entry`（`0x08024544`）与 `el3_exit`（`0x0802dc38`）：按 `bl33_ep->spsr = 0x3C5` 配置 `scr_el3`（`RW=1, NS=1`）、`spsr_el3 = 0x3C5`、`elr_el3 = bl33_ep->pc`，执行 `eret` 进入 AArch64 EL1h 内核。

---

## 3. AArch64 最小化镜像与 DTB 传参方案

由于原厂 U-Boot 的 `start_atf(64, ...)` 固定传入 `bl33_ep->args.arg0 = 0`，而主线 Linux ARM64 引导协议要求 `x0 = DTB 物理地址`、`x1 = x2 = x3 = 0` 且 `Image` 位于 2 MiB 对齐地址，[pack_arm64.py](scripts/pack_arm64.py) 构造如下纯 RAM Legacy `uImage`（`IH_ARCH_ARM64`、`IH_TYPE_KERNEL`、`IH_COMP_NONE`，`Load == Entry == 0x01FE0000`，XIP 零拷贝）：

| 物理地址 | 大小 | 内容 |
| --- | --- | --- |
| `0x01FDFFC0` | 64 B | Legacy `uImage` 头（`ih_arch = 22`，`ih_load = ih_ep = 0x01FE0000`） |
| `0x01FE0000` | 4 KiB | 32 字节 AArch64 入口 shim（`ldr x0, =0x01FE1000; mov x1..x3, xzr; ldr x4, =0x02000000; br x4`） |
| `0x01FE1000` | 124 KiB（实占 ~2.1 KiB） | 极简 ARM64 DTB（[hi3798mv320-fiberhome-hg680kd-arm64.dts](src/hi3798mv320-fiberhome-hg680kd-arm64.dts)） |
| `0x02000000` | ~5.0 MiB | 2 MiB 对齐的主线 Linux 6.18.64 `arch/arm64/boot/Image`（内置 AArch64 musl 1.2.5 + BusyBox 1.37.0 initramfs） |

---

## 4. 实机验证结果

使用 [artifacts/arm64-core/uImage](artifacts/arm64-core/uImage) 通过 TFTP 纯 RAM 引导（未刷写 eMMC、未执行 `saveenv`），完整串口日志见 [210-mainline-arm64-core.txt](logs/210-mainline-arm64-core.txt) 和 [211-mainline-arm64-cpuinfo.txt](logs/211-mainline-arm64-cpuinfo.txt)：

- U-Boot 正确识别 `AArch64 Linux Kernel Image (uncompressed)`，输出 `Start ATF with aarch64 kernel...`、`bl33_ep->spsr = 0x3C5`、`Start Warm Reseting`。
- BL31 `lts-v2.8.6` 顺利交接至 AArch64 EL1，PL011 `earlycon` 立即输出 `Booting Linux on physical CPU 0x0000000000 [0x412fd050]`。
- PSCI 1.1 SMC64 成功拉起全部 4 颗 Cortex-A55（`0x000/0x100/0x200/0x300`，`CPU: All CPU(s) started at EL1`），支持 `fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp`。
- 24.00 MHz `arch_sys_counter` 定时器稳定工作（`sleep 2` 板上 uptime 从 `1.26` 增至 `3.26`，主机耗时 `2.03 s`，四核 GICv3 PPI 27 均匀递增）。
- 顺利进入 AArch64 BusyBox 1.37.0 shell，`uname -a` 输出：`Linux (none) 6.18.64-hg680kd-arm64 #1 SMP 2025-12-01 00:00:00 UTC aarch64`。
- 同步重编既有 `arm32` `core` 目标，与 [artifacts/arm32-timer-fix/SHA256SUMS](artifacts/arm32-timer-fix/SHA256SUMS) 逐项一致（见 [212-core-rebuild-verified-arm64.txt](logs/212-core-rebuild-verified-arm64.txt)）。
