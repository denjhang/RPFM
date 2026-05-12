# WS2812 LED 调试记录

> 日期：2026-05-12

## 硬件

淘宝 RP2350 核心板板载 WS2812，GPIO16，4 颗 RGB LED。

## 淘宝 MicroPython 源码（能亮）

```python
@rp2.asm_pio(sideset_init=rp2.PIO.OUT_LOW, out_shiftdir=rp2.PIO.SHIFT_LEFT, autopull=True, pull_thresh=24)
def ws2812():
    T1 = 2; T2 = 5; T3 = 3
    out(x, 1) .side(0) [T3 - 1]
    jmp(not_x, "do_zero") .side(1) [T1 - 1]
    jmp("bitloop") .side(1) [T2 - 1]
    nop() .side(0) [T2 - 1]
```

时序参数：T1=2, T2=5, T3=3，频率 8MHz，`SHIFT_LEFT`，`sideset_init=OUT_LOW`。

## pico-sdk 官方 ws2812.pio（能亮）

```
T1=3, T2=3, T3=4, 800kHz, SHIFT_RIGHT, sideset_init=OUT_HIGH
```

## 踩坑：淘宝源码移植不亮

直接把淘宝 MicroPython 的 T1=2, T2=5, T3=3 移植到 C PIO 程序，WS2812 不亮、不死机。

### 原因分析

1. MicroPython `sideset_init=rp2.PIO.OUT_LOW` 和 `out_shiftdir=rp2.PIO.SHIFT_LEFT` 在 C SDK 的 PIO v3 中需要不同配置方式
2. MicroPython `sm.put(ar, 8)` 的 `shift=8` 参数和 C 的 `pio_sm_put_blocking(pio, sm, val << 8u)` 行为一致，但配合不同的 `out_shiftdir` 会导致数据位序不同
3. 时序参数 T1/T2/T3 和 `out_shiftdir`/`sideset_init` 是配套的，不能只改时序不改移位方向

### 结论

**必须整套使用**——时序参数、移位方向、sideset 初始状态、频率必须一致。不能混搭。

最终采用 pico-sdk 官方 `pico-examples/pio/ws2812/` 方案，经实测可亮。

## 最终方案

使用 `src/ws2812.pio`（从 pico-examples 复制），参数：
- T1=3, T2=3, T3=4
- 频率 800kHz
- `sm_config_set_out_shift(&c, false, true, 24)` — SHIFT_RIGHT, autopull, 24-bit threshold
- `sideset_init=OUT_HIGH`
- `pio_sm_put_blocking(pio, sm, pixel_grb << 8u)` 发送像素

4 颗 LED 对应 CS0-CS3 片选活动指示：
- LED0 = CS0 (YM2413) 绿色
- LED1 = CS1 (YM2151) 蓝色
- LED2 = CS2 (YM2608) 黄色
- LED3 = CS3 (YM2612) 红色

选中芯片写寄存器时亮 100ms，超时后自动熄灭。
