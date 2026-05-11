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

### PIO0 SM0: GPIO0-13（14-bit 连续，数据+读写+地址）

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
GPIO9:     RD#       bit[9]    读信号（低有效）
GPIO10:    A0        bit[10]   地址线0
GPIO11:    A1        bit[11]   地址线1
GPIO12:    A2        bit[12]   地址线2
GPIO13:    A3        bit[13]   地址线3
```

`out pins, 14` 一次原子输出数据+读写+地址。

### GPIO16: 板载 WS2812（跳过）

### PIO1 SM1: GPIO17-19（片选+复位，WS2812 之后）

```
GPIO17:    CS0#                片选0（低有效）
GPIO18:    CS1#                片选1（低有效）
GPIO19:    CS2#                片选2（低有效）
GPIO20:    CS3#                片选3（低有效）
GPIO21:    IC#                 复位信号（低有效）
```

PIO1 用 `set pins` 控制非连续引脚，切换芯片时调用一次。

### 固定引脚

```
GPIO16:    WS2812     板载 RGB LED（不使用，跳过）
GPIO25:    LED        CPU 心跳
```

### PIO 资源分配

| PIO | SM | 用途 | 引脚 |
|-----|-----|------|------|
| PIO0 | SM0 | 14-bit 数据+读写+地址 | GPIO0-13 |
| PIO1 | SM1 | CS0-CS3 + IC# | GPIO17-21 |

GPIO16 WS2812 不参与，跳过。
剩余 PIO 资源：PIO0 SM1-3, PIO1 SM0/SM2-3 可用于未来扩展。

## GPIO 总分配表

| GPIO | 用途 | 接口 |
|------|------|------|
| 0-7 | D0-D7 数据总线 | PIO0 SM0 → U1 74LVC245 → Bus Board |
| 8 | WR# 写信号 | PIO0 SM0 → U2 74LVC245 → Bus Board |
| 9 | RD# 读信号 | PIO0 SM0 → U2 + U1 DIR |
| 10-13 | A0-A3 地址线 | PIO0 SM0 → U2 74LVC245 → Bus Board |
| 14 | SPI1 SCK | SD 卡 |
| 15 | SPI1 MOSI | SD 卡 |
| 16 | WS2812 板载 | 不使用 |
| 17 | CS0# | PIO1 SM1 → Bus Board |
| 18 | CS1# | PIO1 SM1 → Bus Board |
| 19 | CS2# | PIO1 SM1 → Bus Board |
| 20 | CS3# | PIO1 SM1 → Bus Board |
| 21 | IC# 复位 | PIO1 SM1 → Bus Board |
| 22 | SPI0 SCK | ILI9341 |
| 23 | SPI0 MOSI | ILI9341 |
| 24 | SPI0 MISO | ILI9341 |
| 25 | LED 心跳 | CPU |
| 26 | ILI9341 DC | CPU |
| 27 | ILI9341 RST | CPU |
| 28 | ILI9341 CS | CPU |
| 29 | 触摸屏 CS | CPU |
| 30 | SPI1 MISO | SD 卡 |
| 31 | SD 卡 CS | CPU |

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

**U2: 74LVC245 — WR#/RD#/A0-A3 单向输出（6-bit，只用6通道）**

| 74LVC245 通道 | RP2350A 侧 | Bus Board 侧 |
|--------------|-----------|-------------|
| B1/A1 | GPIO8 (WR#) | WR# |
| B2/A2 | GPIO9 (RD#) | RD# |
| B3/A3 | GPIO10 (A0) | A0 |
| B4/A4 | GPIO11 (A1) | A1 |
| B5/A5 | GPIO12 (A2) | A2 |
| B6/A6 | GPIO13 (A3) | A3 |
| B7/A7 | — (空) | — |
| B8/A8 | — (空) | — |

DIR: 接 GND（固定 B→A，RP2350A 输出方向）
OE#: 接 GND（常使能）

### U1 DIR 连接说明

U1 (数据总线) 的 DIR 引脚由 RD# 控制：
- GPIO9 (RD#) → U2 B2/A2 → Bus Board RD#
- 同时 GPIO9 (RD#) → U1 DIR

RD#=1 (写): U1 B→A，数据从 RP2350A 流向 Bus Board
RD#=0 (读): U1 A→B，数据从 Bus Board 流向 RP2350A
GPIO9 一个引脚同时驱动 U2 的 RD# 输出和 U1 的 DIR 方向。

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
  │ GPIO0-13 (PIO0 14-bit: D0-D7 + WR# + RD# + A0-A3)
  │ GPIO17-21 (PIO1: CS0-CS3 + IC#)
  │ GPIO22-24 (SPI0: ILI9341 数据)
  │ GPIO26-28 (ILI9341 控制: DC, RST, CS)
  │ GPIO29    (触摸屏 CS)
  │ GPIO14-15,31 (SPI1: SD 卡)
  │ GPIO16    (WS2812 板载，不用)
  │ GPIO25    (LED 心跳)
  │
  ├─► U1 74LVC245 (D0-D7 双向缓冲)
  │     DIR ← RD#
  │
  ├─► U2 74LVC245 (WR#, RD#, A0-A3 单向缓冲)
  │     DIR = 固定输出
  │
  ├─► GPIO17 (CS0#) ──► Bus Board
  ├─► GPIO18 (CS1#) ──► Bus Board
  ├─► GPIO19 (CS2#) ──► Bus Board
  ├─► GPIO20 (CS3#) ──► Bus Board
  ├─► GPIO21 (IC#)  ──► Bus Board
  │
  ├─► SPI0: ILI9341 触摸屏
  │     SCK=GPIO22, MOSI=GPIO23, MISO=GPIO24
  │     DC=GPIO26, RST=GPIO27, CS_LCD=GPIO28, CS_TOUCH=GPIO29
  │
  ├─► SPI1: SD 卡
  │     SCK=GPIO14, MOSI=GPIO15, MISO=GPIO30, CS_SD=GPIO31
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

## PIO word 格式（14-bit）

```
bit[7:0]   = D0-D7 数据
bit[8]     = WR#    (1=空闲, 0=写脉冲)
bit[9]     = RD#    (1=空闲, 0=读)
bit[10]    = A0
bit[11]    = A1
bit[12]    = A2
bit[13]    = A3
```

空闲状态: D=0xFF, WR#=1, RD#=1, A0-A3=0
`= 0x0300 | 0xFF = 0x03FF`

CS0-CS3 由 PIO1 SM1 单独控制，不在 PIO0 的 14-bit word 内。

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
