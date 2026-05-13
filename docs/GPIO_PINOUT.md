# RPFM GPIO 引脚分配与扩展规划

## 当前硬件：RP2350A 核心板 (GPIO0-28, 29 IO)

### 方案一：完整版（原始设计，保留存档）

完整 14-bit 并行总线 + 4 片选 + WS2812，无屏幕/SD。

| GPIO | 用途 | 说明 |
|------|------|------|
| 0-7 | D0-D7 | 8-bit 数据总线 |
| 8 | WR# | 写信号 (active low) |
| 9 | RD# | 读信号 (active low) |
| 10-13 | A0-A3 | 4-bit 地址线 |
| 16 | WS2812 | RGB LED 片选指示 |
| 17-20 | CS0-CS3 | 4-bit 片选 |
| 21 | IC# | 全局复位 |
| 25 | LED | 板载心跳指示 |

已用 18/29，剩余 11 个 (GPIO14,15,22-24,26-28)。

### 方案二：实验版 — 触摸屏 + SD 卡（砍 CS2/CS3 + A2/A3）

砍掉 CS2、CS3、A2、A3，保留 2 芯片 + 2 地址线，加 ILI9341 触摸屏 + SD 卡。
实验目的：验证 SPI0 屏幕和 SPI1 SD 卡独立总线方案。

| GPIO | 功能 | 分组 |
|------|------|------|
| **0-7** | D0-D7 | 数据总线 |
| **8** | WR# | 总线控制 |
| **9** | RD# | 总线控制 |
| **10** | A0 | 地址线 |
| **11** | A1 | 地址线 |
| **12** | CS0# | 芯片选择 |
| **13** | CS1# | 芯片选择 |
| **14** | IC# | 全局复位 |
| **15** | *(空)* | |
| **16** | WS2812 | RGB LED |
| **17** | SPI0 SCK | 屏+触摸 |
| **18** | SPI0 MOSI | 屏+触摸 |
| **19** | SPI0 MISO | 屏+触摸 |
| **20** | LCD CS | 屏幕 |
| **21** | LCD DC | 屏幕 |
| **22** | LCD RST | 屏幕 |
| **23** | T_CS | 触摸 (轮询，不接 T_IRQ) |
| **24** | SD CS | SD 卡片选 |
| **25** | LED (板载) | 心跳 |
| **26** | SPI1 SCK | SD 卡 |
| **27** | SPI1 MOSI | SD 卡 |
| **28** | SPI1 MISO | SD 卡 |

28/29 已用，GPIO15 空闲。

**砍掉的功能：**
- CS2、CS3 → 只支持 2 个芯片（CS0 + CS1）
- A2、A3 → 只支持 2 根地址线（A0 + A1），YM2413 和 AY8910 够用
- T_IRQ → 触摸用轮询方式，不接中断

**新增功能：**
- SPI0：ILI9341 屏幕 + XPT2046 触摸（共用 SCK/MOSI/MISO，各自 CS）
- SPI1：SD 卡独立总线（不跟屏幕抢带宽，大 VGM 流式播放不卡顿）

**接线明细：**

ILI9341 屏幕 (6 线)：
- SCK → GPIO17, MOSI → GPIO18, MISO → GPIO19
- CS → GPIO20, DC → GPIO21, RST → GPIO22

XPT2046 触摸 (4 线，共用 SPI0)：
- T_CLK → GPIO17, T_DIN → GPIO18, T_DOUT → GPIO19
- T_CS → GPIO23

SD 卡 (4 线，独立 SPI1)：
- SCK → GPIO26, MOSI → GPIO27, MISO → GPIO28
- CS → GPIO24

### 方案三：I2C OLED + SD 卡 + 按键（轻量版）

| GPIO | 用途 | 说明 |
|------|------|------|
| 0-7 | D0-D7 | 8-bit 数据总线 |
| 8 | WR# | 写信号 |
| 9 | RD# | 读信号 |
| 10-13 | A0-A3 | 地址线 |
| 14 | I2C SDA | OLED 屏幕 (SSD1306/SH1106) |
| 15 | I2C SCL | OLED 屏幕 |
| 16 | WS2812 | RGB LED |
| 17-20 | CS0-CS3 | 片选 |
| 21 | IC# | 全局复位 |
| 22 | SPI SCK | SD 卡 |
| 23 | SPI MOSI | SD 卡 |
| 24 | SPI MISO | SD 卡 |
| 25 | LED | 心跳 |
| 26 | SD CS | SD 片选 |
| 27 | BTN 1 | 播放/暂停 |
| 28 | BTN 2 | 下一首/上一首 (长按) |

27/29 已用。

---

## 未来硬件：RP2350B (GPIO0-47, 48 IO, 8MB PSRAM, 16MB Flash)

RP2350B 完整引出 48 个 GPIO，板载 8MB PSRAM + 16MB Flash，一片搞定全部。

### 完整引脚分配

| GPIO | 功能 | 说明 |
|------|------|------|
| **并行总线** | | |
| 0-7 | D0-D7 | 8-bit 数据总线 |
| 8 | WR# | 写信号 (active low) |
| 9 | RD# | 读信号 (active low) |
| 10 | A0 | 地址线 0 |
| 11 | A1 | 地址线 1 |
| 12 | A2 | 地址线 2 |
| 13 | A3 | 地址线 3 |
| **片选与控制** | | |
| 14 | CS0# | YM2413 |
| 15 | CS1# | YM2151 |
| 16 | CS2# | YM2608 |
| 17 | CS3# | YM2612 |
| 18 | IC# | 全局复位 |
| 19 | LED | 心跳指示 |
| 20 | WS2812 | RGB LED 片选指示 |
| **SPI0 — ILI9341 触摸屏** | | |
| 21 | SPI0 SCK | 屏幕时钟 |
| 22 | SPI0 MOSI | 屏幕数据 |
| 23 | SPI0 MISO | 屏幕读回 |
| 24 | LCD CS | 屏幕片选 |
| 25 | LCD DC | 数据/命令 |
| 26 | LCD RST | 屏幕复位 |
| 27 | LCD BL | 背光控制 |
| 28 | T_CLK | 触摸时钟 (XPT2046) |
| 29 | T_CS | 触摸片选 |
| 30 | T_DIN | 触摸数据入 |
| 31 | T_DOUT | 触摸数据出 |
| 32 | T_IRQ | 触摸中断 |
| **SPI1 — SD 卡** | | |
| 33 | SPI1 SCK | SD 时钟 |
| 34 | SPI1 MOSI | SD 数据入 |
| 35 | SPI1 MISO | SD 数据出 |
| 36 | SD CS | SD 片选 |
| 37 | SD DET | SD 卡检测 |
| **USB Host — MIDI 键盘** | | |
| 38 | USB Host D+ | PIO bit-bang (PIO-USB) |
| 39 | USB Host D- | PIO bit-bang (PIO-USB) |
| **MIDI DIN** | | |
| 40 | MIDI TX | UART TX 31250 baud |
| 41 | MIDI RX | UART RX 31250 baud |
| **扩展预留** | | |
| 42 | I2C SDA | 预留 (EEPROM/传感器) |
| 43 | I2C SCL | 预留 |
| 44 | UART TX | 调试串口 |
| 45 | UART RX | 调试串口 |
| 46 | GPIO | 预留 |
| 47 | GPIO | 预留 |

### RP2350B 资源汇总

| 资源 | 数量 | 用途 |
|------|------|------|
| 已用 GPIO | 42 | |
| 预留 GPIO | 6 | I2C/UART/通用 |
| PSRAM | 8 MB | VGM 缓冲、PCM 数据、LVGL 帧缓冲 |
| Flash | 16 MB | 固件 + 预置曲库 |
| Core 0 | | USB Device + SD 卡 + 屏幕 + 触摸 + MIDI |
| Core 1 | | VGM tight loop (不变) |

### USB Host 说明

硬件 USB 跑 Device 模式（连 PC），USB Host 用 PIO bit-bang 模拟（参考 PIO-USB 方案）。
两路 USB 互不干扰，PC 和 MIDI 键盘可同时连接。

### PSRAM 用途

- VGM 整曲缓冲（不再依赖实时流式传输）
- YM2612 DAC / YM2608 ADPCM 采样数据
- LVGL 帧缓冲（320×240×2 = 150 KB，8MB 里随便用）
- 多通道 PCM 混音中间数据

---

## 架构决策：不需要 RTOS

### 理由

1. Core 1 VGM tight loop **不能被抢占**，任何调度延迟破坏时序精度
2. Core 0 的负载远未到需要 RTOS 的程度：USB HID、SD 卡读文件、屏幕刷新都是间歇性操作
3. RTOS 引入的复杂度（栈分配、IPC、优先级倒置）没有收益
4. 裸机超级循环 + cooperative 分时已验证可靠

### Core 分工

- **Core 0**: `tud_task()` + deferred writes + SD 卡 + 屏幕刷新 + 按键轮询
- **Core 1**: bare-metal tight loop, cycle counter, PIO register writes
- **核间通信**: volatile 变量 + ring buffer（已验证可靠）

---

## 扩展路径

```
阶段一（当前，RP2350A）：并行总线 + USB HID + PC 上位机
    ↓
阶段二（RP2350A 实验）：砍 CS2/CS3 + A2/A3，加触摸屏 + SD 卡
    ↓
阶段三（RP2350A 轻量）：I2C OLED + SD 卡 + 按键（脱机播放）
    ↓
阶段四（RP2350B）：完整引脚，触摸屏 + SD + USB Host MIDI + MIDI DIN
                  独立播放器，不依赖 PC
```

每个阶段固件架构不变，只增加外设初始化和 UI 层。
