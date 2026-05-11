# SCCI/SPFM 协议实现记录

## 状态：已成功 (2026-05-12)

SCCI 握手成功，YM2413 寄存器写入正常，MDPlayer 通过 SCCI 播放 VGM 音乐出声。

## 最终方案

基于 YM2413 演示版最小改动，完全参考 ym2151-rp2040.txt 的协议解析逻辑。
PIO 全并行总线驱动，D0-D7 + WR# + A0 + CS0# 全部由 PIO 原子输出。

### 改动文件

| 文件 | 改动 |
|------|------|
| `CMakeLists.txt` | 加 `PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1` |
| `src/main.c` | 加 `#include "tusb.h"`，SPFM 协议解析，移除 bitbang 和 CPU 总线控制 |
| `src/ym2413.pio` | 11-bit 全并行输出：D0-D7 + WR# + A0 + CS0# |

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
GPIO0-7:   D0-D7 (PIO, 11-bit 并行)
GPIO8:     WR#   (PIO)
GPIO9:     A0    (PIO)
GPIO10:    CS0#  (PIO)
GPIO11:    IC#   (CPU)
GPIO12-14: A1-A3 (CPU, YM2413 未用)
GPIO25:    LED   (CPU 心跳)
```

## PIO 全并行总线

`ym2413.pio` 控制 GPIO0-10 共 11 个引脚，PIO word 格式：

```
bit[7:0]  = D0-D7 数据
bit[8]    = WR# (1=空闲高, 0=写脉冲低)
bit[9]    = A0  (0=地址相位, 1=数据相位)
bit[10]   = CS0# (1=未选中, 0=选中)
```

写寄存器流程（C 侧 push 7 个 word）：
1. 地址相位：A0=0, CS0#=0, WR#=1 → WR#=0 → WR#=1（各间隔 20µs）
2. 数据相位：A0=1, CS0#=0, WR#=1 → WR#=0 → WR#=1（各间隔 20µs）
3. 释放：CS0#=1, WR#=1, A0=0, D=0xFF

## 参考源码

- `reference/ym2151-rp2040.txt` — Arduino RP2040 SCCI 实现（双核）
- `reference/20240827_stm32f103_SPFM_DIY/HARDWARE/YMF288/YMF288M.c` — STM32 SPFM 原版
- `reference/CherryUSB-master/` — 备选 USB 栈（未使用）
