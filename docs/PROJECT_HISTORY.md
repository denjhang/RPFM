# RPFM 项目历史

## 从 SPFM 到 RPFM 的进化之路

> 时间跨度：2022年 ~ 2026年
> 核心目标：用微控制器驱动 YM 系列 FM 音源芯片，通过 SCCI 协议与 PC 播放器通信

---

## 第一代：IAP-SPFM（2022）

**硬件：** FM塔 SPFM 初代硬件
**MCU：** STC 8 位单片机（低成本方案，STC12C5A60S2 系列）
**协议：** SCCI 1.0

初代 SPFM 硬件，奠定了整个项目的架构基础：
- SPFM 总线定义（D0-D7 + WR# + A0-A3 + CS0-CS3）
- SCCI 握手协议（0xFF → "RS"，0xFE → "OK"）
- 多芯片片选复用数据总线

```
PC ──USB CDC──→ STC 8-bit ──GPIO bitbang──→ YM2413
```

## 第二代：IAP-SPFM 二代改版（2022）

**硬件改版：** IAP SPFM 二代硬件、SPFM Light
**MCU：** STC 8 位单片机
**源码路径：** `IAP SPFM-二代硬件改版/iap-src/main.c`（2022.12.10）

硬件迭代，改进 PCB 设计和 USB 接口。

## 第三代：IAP-RESPFM（2023 年底）

**硬件：** DIP40 版本 → TQFP64 封装
**MCU：** STC 8 位单片机（STC12C5A60S2）
**源码路径：** `IAP-RESPFM-自制3代/DIP40版本/SPFM(SV231210)/`
**代码：** `YMF288M.c` / `YMF288M.h` / `Uart0.c` / `Delay.c` / `main.c`
**文件日期：** 2023.11 - 2023.12

版本号 `SV231210` 表示 2023年12月10日。代码中明确使用 `STC12C5A60S2.h` 头文件。
从 IAP-SPFM 升级为 IAP-RESPFM，PCB 进入可生产状态，SPFM 项目从实验原型转向产品化。

## RE2-RESPFM 系列（2024）

### RE2 总线板迭代

```
RE2e BUS v0.1 → v0.2(废弃) → v0.3s(废弃) → v0.4(废弃) → v0.5(废弃) → v0.55(废弃) → v0.6
  → RESPFM Bus Board v0.7
```

总线板（Bus Board）是连接多颗 FM 芯片的背板。经历了 7 个版本的迭代，v0.1-v0.55 全部废弃，v0.6 才定型，最终 v0.7 为当前版本。

### STM32-RESPFM（2024.06 - 2024.09）

**MCU：** STM32F103C8
**芯片：** YMF288M（YM2608 兼容）
**源码路径：** `STM32-RESPFM/src/` → `stm32-respfm-20240910/`

PCB 迭代了 4 版：
```
v0.1 (2024.06.11-12) → v0.2 (2024.08.21) → v0.3 (2024.09.02) → v0.35 (2024.09.03)
```

固件迭代从 `YMF288M.hex` 到 `YMF288M40.hex`，共 40+ 个 hex 版本：
```
YMF288M1.hex  (2024.02.01)
YMF288M.hex   (2024.02.05)
YMF288M 2024.6.11.hex
YMF288M P9-10 2024.6.17.hex
YMF288M 握手 2024.6.17.hex
...
YMF288M37(YMF276+SN76489完美).hex (2024.08.19)
YMF288M-2024.6.26.hex
YMF288M2024.8.11.hex
```

关键里程碑：
- YMF288M37: YMF276 + SN76489 完美驱动
- 驱动代码使用 `nops.inc` 汇编精确延时（手动数时钟周期）
- `dev_uart.c` / `fifo.c` / `timer2.c` / `YMF288M.c` 模块化架构

### STM32 SPFM DIY 参考实现（2024.08）

**路径：** `reference/20240827_stm32f103_SPFM_DIY/`
**文件：** `YMF288M.c` / `YMF288M.h` / `dev_uart.c` / `fifo.c` / `nops.inc`

这个版本成为 RPFM 项目的核心参考源码。SCCI 协议解析逻辑直接移植自此处。

### PIC18F25K22-RESPFM（2024.12 - 2025.01）

**MCU：** PIC18F25K22
**版本：** v0.1, v0.2

尝试换 MCU 平台，未继续。

## RPFM：RP2350A 时代（2026.05）

**MCU：** RP2350A（Raspberry Pi Pico 2）
**芯片：** YM2413 OPLL（当前验证）
**SDK：** pico-sdk 2.2.0 + TinyUSB 0.20.0

### 关键技术突破

| 对比项 | IAP-SPFM (STC 8-bit) | STM32-RESPFM | RPFM |
|--------|---------------------|-------------|------|
| MCU | STC 8 位单片机 | STM32F103C8 | RP2350A |
| 总线驱动 | GPIO bitbang | GPIO bitbang + nops.inc 汇编 | PIO 11-bit 原子输出 |
| 时序精度 | 手动延时 | 手动数时钟周期，中断会破坏时序 | PIO 8ns 分辨率，不受中断影响 |
| USB 栈 | — | HAL USB CDC，调通耗时数周 | pico-sdk + TinyUSB 开箱即用 |
| 协议解析 | 从零实现 SCCI | 从零实现 SCCI | 直接移植 ym2151-rp2040 参考实现 |
| 开发周期 | 数月 | ~2个月达到完美驱动 OPLL | 几天出声 + 全并行总线 |

### 开发时间线

```
2026.05.10  项目启动，pico-sdk 1.5.1 环境搭建
2026.05.10  YM2413 PIO 驱动出声（GPIO0-8: D0-D7 + WR#）
2026.05.11  USB CDC SCCI 握手成功
2026.05.11  修复死锁：putchar_raw → tud_cdc_write
2026.05.11  修复无声音：寄存器地址缓存
2026.05.11  MDPlayer 通过 SCCI 播放 VGM 音乐出声
2026.05.12  目录整理：src/, scripts/, docs/
2026.05.12  PIO 全并行总线：A0/CS0# 纳入 PIO（GPIO0-10）
```

## 项目源码位置索引

| 项目 | MCU | 路径 | 时间 |
|------|-----|------|------|
| FM塔 SPFM 初代 | STC 8-bit | `SPFM/FM塔 SPFM-初代硬件/` | 2022 |
| IAP SPFM 二代 | STC 8-bit | `SPFM/IAP SPFM-二代硬件改版/` | 2022 |
| IAP-RESPFM 三代 | STC 8-bit (STC12C5A60S2) | `SPFM/IAP-RESPFM-自制3代/` | 2023.11-12 |
| STM32-RESPFM | STM32F103 | `RE2-RESPFM/STM32-RESPFM/` | 2024.06-09 |
| STM32 SPFM DIY 参考 | STM32F103 | `reference/20240827_stm32f103_SPFM_DIY/` | 2024.08 |
| PIC18F 尝试 | PIC18F25K22 | `RE2-RESPFM/PIC18F25K22-RESPFM/` | 2024.12 |
| **RPFM（当前）** | RP2350A | `RPFM/` | 2026.05 |

## 经验总结

1. **协议知识是核心资产** — SCCI 握手、寄存器写入、地址缓存等经验从 2022 年 STC 时代积累至今，直接移植到 RP2350A
2. **硬件工具的进化** — 从 STC 8-bit 到 STM32 再到 RP2350A PIO，每次平台升级都大幅缩短开发周期
3. **Bus Board 的教训** — 7 个版本迭代说明总线板设计的复杂度，未来 RPFM 的 PCB 需要提前规划好多芯片布线
4. **nops.inc 精神** — 手动数时钟周期是嵌入式开发者的最基本功，但好的工具链让你把精力放在更有价值的地方
