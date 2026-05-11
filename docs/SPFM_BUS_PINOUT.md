# RPFM 完整 SPFM 总线引脚分配与硬件设计

## 设计目标

RPFM 作为 RE:Birth 主控板，通过 RESPFM Bus Board 连接所有 RE2 系列声卡模块板。
需要完整实现 SPFM 总线所有信号，支持多芯片（YM2413/YM2151/YM2608/YM2612 等）并行驱动。

## SPFM 总线信号定义

### 总线信号列表（17 个）

| 信号 | 方向 | 功能 |
|------|------|------|
| D0-D7 | 双向 | 数据总线（8-bit） |
| WR# | 输出 | 写信号（低有效） |
| RD# | 输出 | 读信号（低有效） |
| A0 | 输出 | 地址/数据选择 |
| A1 | 输出 | 地址线1 |
| A2 | 输出 | 地址线2 |
| A3 | 输出 | 地址线3 |
| CS0# | 输出 | 片选0 |
| CS1# | 输出 | 片选1 |
| CS2# | 输出 | 片选2 |
| CS3# | 输出 | 片选3 |

### 辅助信号（非总线）

| 信号 | 方向 | 功能 |
|------|------|------|
| IC# | 输出 | 复位信号（低有效） |
| LED | 输出 | 板载 LED 心跳 |
| WS2812 | 输出 | 板载 RGB LED（GPIO16 固定） |

## RP2350A 引脚分配

### PIO0 SM0: GPIO0-15（16-bit 连续，写+读总线核心）

```
GPIO0:     D0        bit[0]    数据位0
GPIO1:     D1        bit[1]    数据位1
GPIO2:     D2        bit[2]    数据位2
GPIO3:     D3        bit[3]    数据位3
GPIO4:     D4        bit[4]    数据位4
GPIO5:     D5        bit[5]    数据位5
GPIO6:     D6        bit[6]    数据位6
GPIO7:     D7        bit[7]    数据位7
GPIO8:     WR#       bit[8]    写信号（低有效）
GPIO9:     A0        bit[9]    地址/数据选择
GPIO10:    CS0#      bit[10]   片选0（低有效）
GPIO11:    CS1#      bit[11]   片选1（低有效）
GPIO12:    A1        bit[12]   地址线1
GPIO13:    A2        bit[13]   地址线2
GPIO14:    A3        bit[14]   地址线3
GPIO15:    RD#       bit[15]   读信号（低有效）
```

`out pins, 16` 一次原子输出所有信号。

### PIO1 SM1: GPIO17-19（辅助信号，非连续引脚）

PIO1 不要求引脚连续（用 `set pins` / `set pindirs` 指令控制）：

```
GPIO17:    CS2#                片选2（低有效）
GPIO18:    CS3#                片选3（低有效）
GPIO19:    IC#                 复位信号（低有效）
```

CS2#/CS3# 在切换芯片时调用一次 PIO `set` 即可。
IC# 仅在复位时使用。

### 固定引脚

```
GPIO16:    WS2812     板载 RGB LED（不使用）
GPIO25:    LED        CPU 心跳
```

### PIO 资源分配

| PIO | SM | 用途 | 引脚 |
|-----|-----|------|------|
| PIO0 | SM0 | 16-bit 总线 (D0-D7, WR#, RD#, A0-A3, CS0-CS1) | GPIO0-15 |
| PIO1 | SM1 | CS2#, CS3#, IC# 辅助信号 | GPIO17-19 |

GPIO16 WS2812 不参与本项目，PIO1 SM0 空闲。
剩余 PIO 资源：PIO0 SM1-3, PIO1 SM0/SM2-3 可用于未来扩展。

### GPIO25: 板载 LED（CPU 心跳）

## 74LVC245 双向缓冲器

连接 RESPFM Bus Board 需要 2 片 74LVC245：

### 电平转换需求

RP2350A GPIO 工作电压 3.3V，YM 系列芯片多为 5V。
74LVC245 支持 3.3V ↔ 5V 双向电平转换，同时提供总线驱动能力。

### 缓冲器分配

**U1: 74LVC245 — D0-D7 双向数据总线（8-bit）**

| 74LVC245 通道 | RP2350A 侧 | Bus Board 侧 |
|--------------|-----------|-------------|
| B1/A1 | GPIO0 (D0) | D0 |
| B2/A2 | GPIO1 (D1) | D1 |
| B3/A3 | GPIO2 (D2) | D2 |
| B4/A4 | GPIO3 (D3) | D3 |
| B5/A5 | GPIO4 (D4) | D4 |
| B6/A6 | GPIO5 (D5) | D5 |
| B7/A7 | GPIO6 (D6) | D6 |
| B8/A8 | GPIO7 (D7) | D7 |

DIR: 由 GPIO15 (RD#) 控制
  - RD#=1 (写): B→A，RP2350A 输出数据到 Bus Board
  - RD#=0 (读): A→B，Bus Board 数据回读到 RP2350A
  - GPIO15 通过 U2 输出到 Bus Board 的 RD#，同时连到 U1 的 DIR
OE#: 接 GND（常使能）

**U2: 74LVC245 — 控制信号单向输出（8-bit）**

| 74LVC245 通道 | RP2350A 侧 | Bus Board 侧 |
|--------------|-----------|-------------|
| B1/A1 | GPIO8 (WR#) | WR# |
| B2/A2 | GPIO9 (A0) | A0 |
| B3/A3 | GPIO10 (CS0#) | CS0# |
| B4/A4 | GPIO11 (CS1#) | CS1# |
| B5/A5 | GPIO12 (A1) | A1 |
| B6/A6 | GPIO13 (A2) | A2 |
| B7/A7 | GPIO14 (A3) | A3 |
| B8/A8 | GPIO15 (RD#) | RD# |

DIR: 接 GND（固定 B→A，RP2350A 输出方向）
OE#: 接 GND（常使能）

### 电源

```
RP2350A 侧 VCC: 3.3V
Bus Board 侧 VCC: 5V（由外部供电或从 Bus Board 取电）
GND: 共地
```

## 完整系统连接图

```
PC/MDPlayer
  │
  │ USB CDC (SCCI 协议)
  ▼
RP2350A (RPFM 主控)
  │
  │ GPIO0-15 (PIO0 16-bit 并行)
  │ GPIO17-19 (CPU: CS2#, CS3#, IC#)
  │ GPIO16 (PIO1: WS2812)
  │ GPIO25 (CPU: LED)
  │
  ├─► U1 74LVC245 (D0-D7 双向缓冲)
  │     DIR ← RD#
  │
  ├─► U2 74LVC245 (WR#,A0-A3,CS0#,CS1#,RD# 单向缓冲)
  │     DIR = 固定输出
  │
  ├─► GPIO17 (CS2#) ──直接或缓冲──→ Bus Board
  ├─► GPIO18 (CS3#) ──直接或缓冲──→ Bus Board
  ├─► GPIO19 (IC#)  ──直接或缓冲──→ Bus Board
  │
  ▼
RESPFM Bus Board v0.7 (5V 总线)
  │
  ├── RE2-YM2413   (CS0#)
  ├── RE2-YM2151   (CS1#)
  ├── RE2-YM2608   (CS2#, A1)
  ├── RE2-YM2612   (CS3#, A1, A2)
  ├── RE2-SN76489  (A3=0, WR# only)
  └── ...
```

## PIO word 格式（16-bit）

```
bit[7:0]   = D0-D7 数据
bit[8]     = WR#    (1=空闲, 0=写脉冲)
bit[9]     = A0     (0=地址, 1=数据)
bit[10]    = CS0#   (1=未选, 0=选中)
bit[11]    = CS1#   (1=未选, 0=选中)
bit[12]    = A1
bit[13]    = A2
bit[14]    = A3
bit[15]    = RD#    (1=空闲, 0=读)
```

空闲状态: D=0xFF, WR#=1, RD#=1, A0-A3=0, CS0#-CS1#=1
`= 0x8300 | 0xFF = 0x83FF`

## 各芯片总线信号映射

| 芯片 | CS# | A0 | A1 | A2 | A3 | 说明 |
|------|-----|----|----|----|----|------|
| YM2413 (OPLL) | CS0# | A0 | - | - | - | A0=0写地址, A0=1写数据 |
| YM2151 (OPM) | CS1# | A0 | - | - | - | A0=0写地址, A0=1写数据 |
| YM2608 (OPNA) | CS2# | A0 | A1 | - | - | A1=0 FM, A1=1 SSG/ADPCM |
| YM2612 (OPN2) | CS3# | A0 | A1 | A2 | - | A1+A2 选择 bank |
| SN76489 | CS0# | - | - | - | - | 仅 WR#，无 A0 |
| YMF288 (OPN3L) | CS2# | A0 | A1 | - | - | 兼容 YM2608 |

## 后续扩展

- CS2#/CS3# 如需原子化可改用 PIO1 SM1 控制
- SN76489 写入需要 18µs WR# 低电平保持，可在 PIO 程序中用 nop 延时
- YM2612 DAC 模式需要连续数据流，可能需要 DMA + PIO 配合
