# RPFM GPIO 引脚分配与扩展规划

## 当前硬件：RP2350A 核心板 (GPIO0-28, 29 IO)

### 当前方案：并行总线 + HID，无屏幕/SD

| GPIO | 用途 | 说明 |
|------|------|------|
| 0-7 | D0-D7 | 8-bit 数据总线 |
| 8 | WR# | 写信号 (active low) |
| 9 | RD# | 读信号 (active low) |
| 10-13 | A0-A3 | 地址线 |
| 16 | WS2812 | RGB LED 片选指示 |
| 17-20 | CS0-CS3 | 4-bit 片选 |
| 21 | IC# | 全局复位 |
| 25 | LED | 心跳指示 |

已用 18/29，剩余 11 个 (GPIO14,15,22-24,26-28)。

### 阶段二：加 I2C OLED + SD 卡 + 2 按键

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
| 28 | BTN 2 | 上一首/下一首 (长按) |

27/29 已用。I2C 屏幕 2 线 + SPI SD 卡 4 线 + 2 按键 = 8 个额外 GPIO。

4 颗按键不够空间，2 颗按键用长按/短按区分功能：
- BTN 1 短按 = 播放/暂停
- BTN 2 短按 = 下一首，长按 = 上一首

### 如果只要 I2C 屏幕 + 4 按键（不要 SD 卡）

| GPIO | 用途 |
|------|------|
| 14 | I2C SDA |
| 15 | I2C SCL |
| 22 | BTN 1 |
| 23 | BTN 2 |
| 24 | BTN 3 |
| 25 | LED + BTN 4 (复用) |

更宽裕，剩余 GPIO26-28 + 22-24(不用按钮时) 留扩展。

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
阶段二（RP2350A）：+ I2C OLED + SD 卡 + 2 按键（脱机播放）
    ↓
阶段三（RP2350B）：+ ILI9341 触摸屏 + SD 卡 + USB Host MIDI + MIDI DIN
                  独立播放器，不依赖 PC
```

每个阶段固件架构不变，只增加外设初始化和 UI 层。
