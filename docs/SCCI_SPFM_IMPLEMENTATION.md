# SCCI/SPFM 协议实现记录

## 状态：已成功 (2026-05-12)

SCCI 握手成功，YM2413 寄存器写入正常，MDPlayer 通过 SCCI 播放 VGM 音乐出声。

## 最终方案

基于 YM2413 演示版最小改动，完全参考 ym2151-rp2040.txt 的协议解析逻辑。
PIO0 全并行总线驱动 D0-D7 + WR# + RD# + A0-A3（14-bit），PIO1 驱动 CS0-CS3（4-bit）。

### 改动文件

| 文件 | 改动 |
|------|------|
| `CMakeLists.txt` | 加 `PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1` |
| `src/main.c` | 加 `#include "tusb.h"`，SPFM 协议解析，移除 bitbang 和 CPU 总线控制 |
| `src/ym2413.pio` | 14-bit 全并行输出：D0-D7 + WR# + RD# + A0-A3 |
| `src/pio_cs.pio` | 4-bit CS 输出：CS0-CS3（PIO1 SM1） |

### 核心设计决策

**发送：`tud_cdc_write()` + `tud_cdc_write_flush()`**
- 直接调用 TinyUSB API，绕过 pico-sdk stdio 层
- pico-sdk 的 `stdio_usb_out_chars` 有 `stdio_usb_mutex`，与 `getchar_timeout_us` 争抢会死锁
- `putchar_raw()` 在 `WITHOUT_DTR=1` 下会导致握手后死机

**接收：`getchar_timeout_us(0)`**
- stdio 层读 CDC 数据，有 mutex 但只读不发送所以不阻塞

**`PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1`**
- SCCI 不设 DTR 信号，不加这个 `stdio_usb_connected()` 返回 false
- `putchar_raw` 内部检查 connected 状态后静默丢弃数据

### 协议状态机 (ym2151 style)

```
scci_parse_idx:
  0 → 收到 0xFF: 回 "RS"，收到 0xFE: 复位回 "OK"，收到 0x0n: 记 slot，进 1
  1 → 解析命令高4位 (0x00/0x80/0x20)，记 scci_a，进 2
  2 → cmd=0x00: 缓存地址 scci_addr，进 3
      cmd=0x80: ym2413_write_reg(scci_a, data)，回 0
      cmd=0x20: SN76489（暂未实现），回 0
  3 → cmd=0x00: ym2413_write_reg(scci_addr, data)，回 0
```

### 关键 bug：寄存器地址缓存

`ym2413_write_reg(reg, data)` 一步完成 A0=0 写地址 + A0=1 写数据。
SCCI 的 cmd=0x00 发两字节（地址+数据），必须在 parse_idx==2 缓存地址，
parse_idx==3 一次调用 `ym2413_write_reg`。分两次调用会写入错误数据。

## 踩坑记录

| 问题 | 原因 | 解决 |
|------|------|------|
| SCCI 不发数据到设备 | 没设 WITHOUT_DTR，stdio_usb_connected() 返回 false | CMake 加 PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1 |
| 握手后死机 LED 常亮 | putchar_raw 经过 stdio_usb_mutex 死锁 | 改用 tud_cdc_write() |
| fflush(stdout) 死机 | 阻塞等待 USB 传输完成，tud_task 没被调用 | 不用 fflush |
| 双重 getchar_timeout_us | 协议和 main 各读一次，字节处理两次 | 只在一个地方读 |
| tud_cdc_write 不 flush | 数据留在 FIFO 不发，SCCI 收不到回复 | 加 tud_cdc_write_flush() |
| 手动 pll_init 配时钟 | 破坏 SDK 已配好的系统时钟，开机死机 | 不重复配置，让 SDK 处理 |
| 寄存器写入没声音 | ym2413_write_reg 一步完成 addr+data，不能分两次调 | 缓存地址，最后一步写入 |

## 硬件接线

```
PIO0 SM0 (14-bit, GPIO0-13):
GPIO0-7:   D0-D7 (PIO)
GPIO8:     WR#   (PIO)
GPIO9:     RD#   (PIO)
GPIO10-13: A0-A3 (PIO)

PIO1 SM1 (4-bit, GPIO17-20):
GPIO17-20: CS0-CS3 (PIO)

GPIO21:    IC#   (CPU, 上电复位)
GPIO25:    LED   (CPU 心跳)
GPIO16:    WS2812 (板载, 不使用)
```

## PIO 全并行总线

### PIO0: 数据+控制总线（14-bit）

`ym2413.pio` 控制 GPIO0-13 共 14 个引脚，PIO word 格式：

```
bit[7:0]   = D0-D7 数据
bit[8]     = WR# (1=空闲高, 0=写脉冲低)
bit[9]     = RD# (1=空闲高, 0=读)
bits[13:10]= A3 A2 A1 A0
```

空闲状态: `0x03FF` (D=0xFF, WR#=1, RD#=1, A0-A3=0)

写寄存器流程（C 侧 push 7 个 word，CS 由 PIO1 单独控制）：
1. 地址相位：A0=0, WR#=1 → WR#=0 → WR#=1（各间隔 20µs）
2. 数据相位：A0=1, WR#=1 → WR#=0 → WR#=1（各间隔 20µs）
3. 释放：WR#=1, A0-A3=0, D=0xFF

### PIO1: 片选控制（4-bit）

`pio_cs.pio` 控制 GPIO17-20，PIO word 格式：

```
bit[0]=CS0#, bit[1]=CS1#, bit[2]=CS2#, bit[3]=CS3#
1=空闲(高), 0=选中(低)
```

空闲状态: `0x0F`。选择芯片 N: `0x0F & ~(1 << N)`

## 参考源码

- `reference/ym2151-rp2040.txt` — Arduino RP2040 SCCI 实现（双核）
- `reference/20240827_stm32f103_SPFM_DIY/HARDWARE/YMF288/YMF288M.c` — STM32 SPFM 原版
- `reference/CherryUSB-master/` — 备选 USB 栈（未使用）
