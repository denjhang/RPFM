# WS2812 LED 调试记录

> 日期：2026-05-12
> 硬件：淘宝 RP2350 核心板，板载 WS2812，GPIO16，4 颗 RGB LED 串联

## 关键经验

### 1. PIO 程序必须整套使用

淘宝 MicroPython 源码和 pico-sdk 官方 pico-examples 用的是**完全不同的 PIO 配置**：

| 参数 | 淘宝 MicroPython | pico-examples 官方 |
|------|-----------------|-------------------|
| T1 | 2 | 3 |
| T2 | 5 | 3 |
| T3 | 3 | 4 |
| 频率 | 8MHz | 800kHz |
| out_shiftdir | SHIFT_LEFT | SHIFT_RIGHT |
| sideset_init | OUT_LOW | OUT_HIGH |

**不能混搭。** 把淘宝的 T1/T2/T3 移植到 C SDK 但不改 `out_shiftdir` 和 `sideset_init`，结果不亮不死机。

最终用 pico-examples 官方方案，实测可亮。`src/ws2812.pio` 直接从官方复制。

### 2. 必须发完整帧

WS2812 是串联协议，4 颗 LED 必须每次发 4 个像素（4 × 24 bit = 96 bit）。

```c
// 正确：每次发 4 个像素
static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) {
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
    }
}
```

**只控制单个 LED 时，其他 LED 发 0（关闭），但必须发。** 不亮的 LED 发 0，不能跳过。

### 3. 亮度控制

WS2812 亮度由 R/G/B 值直接控制：

```c
// 全亮度红色
0xFF0000   // R=0xFF, G=0x00, B=0x00 (GRB 格式)

// 半亮度红色
0x800000   // R=0x80, G=0x00, B=0x00

// 10% 亮度红色
0x190000   // R=0x19, G=0x00, B=0x00
```

GRB 格式（WS2812 标准）：`bits[23:16]=G, bits[15:8]=R, bits[7:0]=B`

```c
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}
```

当前 CS 指示灯亮度 ~40%（0x66），启动动画亮度 ~6%（0x10）。

### 4. 数据格式：`pixel_grb << 8u`

```c
// pico-examples 官方做法
pio_sm_put_blocking(pio, sm, pixel_grb << 8u);
```

`<< 8u` 是因为 PIO `out_shift` 配置为 24-bit autopull，32-bit FIFO word 的低 24 位有效，高 8 位被丢弃。`pixel_grb` 本身是 24 位值，左移 8 位后高 24 位就是有效数据。

### 5. 关灯必须同步数组状态

```c
// 错误：直接发 0，数组没清零
static inline void ws_led_off_all(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++)
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, 0);
}

// 正确：清零数组后统一更新
static inline void ws_led_off_all(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update(); // 通过数组发送，保持一致
}
```

如果 `ws_led_off_all` 不清 `s_ws_leds[]` 数组，下次 `ws_led_on(slot)` 调 `ws2812_update` 时会把残留的颜色值重新发送，导致所有 LED 亮。

### 6. Init 阶段避免触发 LED

`s_ws_ready` 标志在 `ym2413_reset()` / `ym2413_mute_all()` 之后才设为 true。否则 init 阶段每次 `ym2413_write_reg` 都会触发 WS2812 发送，导致红灯常亮 ~500ms（mute 写 19 个寄存器）。

## 踩坑记录

| 问题 | 原因 | 解决 |
|------|------|------|
| 淘宝源码移植不亮 | 时序/移位方向/sideset 混搭 | 用官方 pico-examples 完整方案 |
| `pio_instr_mem_write` 链接失败 | RP2350 PIO v3 无此函数 | CS PIO 改用 `.pio` 文件 |
| 4 个 LED 同时亮 | `ws_led_off_all` 没清 `s_ws_leds` 数组 | 清零数组后统一 `ws2812_update` |
| Init 阶段红灯常亮 | `s_ws_ready` 在 reset/mute 前设 true | mute 完再设 true |
| 启动灯太亮 | 颜色值 0xFF | 降到 0x10（~6%）和 0x66（~40%） |

## 当前功能

### 启动动画
上电后 6 色流水灯渐变（~6% 亮度），然后熄灭。

### 握手闪烁
SCCI 握手成功（收到 0xFF 或 0xFE）时，4 灯按 CS 颜色短闪 2 次（亮 50ms 灭 50ms）。

### CS 片选指示
| LED | 颜色（40% 亮度） | 对应 CS |
|-----|-----------------|---------|
| 0 | 绿 | CS0 (YM2413) |
| 1 | 蓝 | CS1 (YM2151) |
| 2 | 黄 | CS2 (YM2608) |
| 3 | 红 | CS3 (YM2612) |

写寄存器时对应 LED 亮 10ms，超时自动熄灭。

## 文件

- `src/ws2812.pio` — PIO 程序（pico-examples 官方）
- `src/main.c` — WS2812 驱动代码（init/update/on/off）
- 参考：`reference/pico-examples-master/pio/ws2812/`
