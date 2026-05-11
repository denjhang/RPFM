# RPFM 项目历史

## 从 SPFM 到 RPFM 的进化之路

> 时间跨度：2022年 ~ 2026年
> 核心目标：用微控制器驱动 YM 系列 FM 音源芯片，通过 SCCI 协议与 PC 播放器通信

---

## 原版硬件克隆（早期）

通过国内外网友提供的原始硬件，直接提取克隆固件。这些克隆版本已经能正常工作，是整个项目的起点。

### SPFM Light（PIC18F2550）

**MCU：** PIC18F2550
**性质：** 从原版 SPFM Light 硬件提取克隆，非自研
**握手：** 使用 LT 回文（发送 0xFF，回复 "LT"）— 2 卡槽模式

这是最早接触到的 SPFM 固件。LT 代表 2 卡槽（2-slot），RS 代表 4 卡槽（4-slot）。
克隆版功能完整，开箱即用。

### RESPFM（PIC18F25K22）

**MCU：** PIC18F25K22
**性质：** 从原版 RESPFM 硬件提取克隆，非自研

原版 RESPFM 固件直接提取使用。

这两个克隆版本证明 SCCI 协议和 SPFM 总线方案完全可行，为后续自研提供了验证基础。

---

## 协议逆向（2023）

早期克隆版只有固件二进制，不了解协议细节。2023 年通过虚拟串口穷举，
才搞清楚 SCCI 协议的完整握手流程：

- 0xFF → "RS" 回文 — 4 卡槽模式（SCCI 标准）
- 0xFF → "LT" 回文 — 2 卡槽模式（SPFM Light 兼容）
- 0xFE → 复位，回复 "OK"
- 寄存器写入：cmd 0x00（地址+数据两字节）/ cmd 0x80（直接写入）

在此之前只知道 LT 回文（2 卡槽），RS（4 卡槽）是穷举出来的。这个突破让自研固件成为可能。

---

## 自研系列

### IAP-SPFM（2022）

**硬件：** FM塔 SPFM 初代硬件
**MCU：** STC 8 位单片机（低成本方案，STC12C5A60S2 系列）
**协议：** SCCI 1.0（兼容 LT 回文）

初代自研 SPFM 硬件，奠定了整个项目的架构基础：
- SPFM 总线定义（D0-D7 + WR# + A0-A3 + CS0-CS3）
- SCCI 握手协议
- 多芯片片选复用数据总线

```
PC ──USB CDC──→ STC 8-bit ──GPIO bitbang──→ YM2413
```

### IAP-SPFM 二代改版（2022）

**硬件改版：** IAP SPFM 二代硬件
**MCU：** STC 8 位单片机
**源码路径：** `IAP SPFM-二代硬件改版/iap-src/main.c`（2022.12.10）

硬件迭代，改进 PCB 设计和 USB 接口。

### IAP-RESPFM（2023 年底）

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

## RE2 声卡模块板系列

**RE2 = Project RE:Birth 2nd**

全部由 Denjhang 自主设计 PCB。每块模块板对应一颗或一组音源芯片，
通过 SPFM 总线与主控板连接，所有模块板共享统一的 RE2 Bus Board 背板接口。

**路径：** `自制模块板/`

### 模块板开发时间线（按文件日期排序）

```
2023.12  RE2-SPFM Tower            SPFM 塔式扩展
2023.12  RE2-SPFM_Light            SPFM Light 兼容板

2024.09  RE2-YM2151                YM2151 (OPM) 8通道FM
2024.09  RE2-YM2608                YM2608 (OPNA) 6FM+3SSG+ADPCM
2024.09  RE2-YMF262                YMF262 (OPL3) 4算子FM
2024.09  RE2-YM2203                YM2203 (OPN) 3FM+3SSG

2024.10  RE2-AYB03                 2×OPLL + SSG 复合音源
2024.10  RE2-Y8950                 Y8950 (MSX-AUDIO) OPL1+ADPCM
2024.10  RE2-YM3302                YM3302 稀有芯片
2024.10  RE2-YM3806                YM3806 稀有芯片
2024.10  RE2-YMU762                YMU762 手机音源
2024.10  RE2-YMZ284-294            YMZ284/YMZ294 PSG变体
2024.10  RE2-AY8910-YMZ284         AY-3-8910/YMZ284 PSG方波
2024.10  RE2-i8253                 Intel 8253 可编程定时器
2024.10  RE2-SN76489               SN76489 PSG方波
2024.10  RE2-2xYM3812              2×YM3812 双OPLL
2024.10  RE2-YM3812_3526           YM3812 (OPLL2)
2024.10  RE2-YMF276-288-289        YMF288 (OPN3L) YM2608兼容
2024.10  RE2-NRTBD                 定制模块

2024.11  RE2-NBV5                  2×OPM + SSG 复合音源

2025.01  RE2-YM2612-3438           YM2612 (OPN2) 6FM+DAC
2025.01  RE2-SDQ1                  定制模块
2025.01  RE2-YM3427                YM3427 稀有芯片

2025.02  RE2-YM7129                YM7129 空间处理器
2025.02  RE2-M208B1                定制模块
2025.02  RE2-TLC7524               TLC7524 DAC
2025.02  RE2-YMZ261-285            YMZ261/YMZ285 稀有芯片
2025.02  RE2-YMZ280B               YMZ280B PCM音源

2025.03  RE2-YM2163                YM2163 (OPP) 雅马哈键盘音源
2025.03  RE2-SAA1099               SAA1099 双声道方波
2025.03  RE2-YMF297                YMF297 (OPL4) OPL3+波形表

2025.04  RE2-RESPFM                RE:Birth主控板
2025.05  RE2-YM2413                YM2413 (OPLL) 9通道旋律+5节奏
```

## 项目源码位置索引

| 项目 | MCU | 性质 | 路径 | 时间 |
|------|-----|------|------|------|
| SPFM Light 克隆 | PIC18F2550 | 提取克隆 | `SPFM/SPFM light-第二代硬件/` | 早期 |
| RESPFM 克隆 | PIC18F25K22 | 提取克隆 | — | 早期 |
| FM塔 SPFM 初代 | STC 8-bit | 自研 | `SPFM/FM塔 SPFM-初代硬件/` | 2022 |
| IAP SPFM 二代 | STC 8-bit | 自研 | `SPFM/IAP SPFM-二代硬件改版/` | 2022 |
| IAP-RESPFM 三代 | STC 8-bit (STC12C5A60S2) | 自研 | `SPFM/IAP-RESPFM-自制3代/` | 2023.11-12 |
| STM32-RESPFM | STM32F103 | 自研 | `RE2-RESPFM/STM32-RESPFM/` | 2024.06-09 |
| STM32 SPFM DIY 参考 | STM32F103 | 自研 | `reference/20240827_stm32f103_SPFM_DIY/` | 2024.08 |
| PIC18F25K22-RESPFM | PIC18F25K22 | 自研 | `RE2-RESPFM/PIC18F25K22-RESPFM/` | 2024.12 |
| **RPFM（当前）** | RP2350A | 自研 | `RPFM/` | 2026.05 |

## 经验总结

1. **协议知识是核心资产** — SCCI 握手、寄存器写入、地址缓存等经验从 2022 年 STC 时代积累至今，直接移植到 RP2350A
2. **硬件工具的进化** — 从 STC 8-bit 到 STM32 再到 RP2350A PIO，每次平台升级都大幅缩短开发周期
3. **Bus Board 的教训** — 7 个版本迭代说明总线板设计的复杂度，未来 RPFM 的 PCB 需要提前规划好多芯片布线
4. **nops.inc 精神** — 手动数时钟周期是嵌入式开发者的最基本功，但好的工具链让你把精力放在更有价值的地方
