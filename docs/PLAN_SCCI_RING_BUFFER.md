# RPFM SCCI 环形缓冲区 + 时间轴驱动

> 日期：2026-05-12
> 状态：待实施

## Context

当前 RPFM 逐字节透传 SCCI，UART 速率波动直接变成时序抖动。
AY8910 大数据量 VGM 播放速度不稳定。

SCCI 协议里 0x80 就是"等一个 tick"。
连续多个 0x80 = 等多个 tick。这就是 SCCI 的时间轴。

方案：环形缓冲区存储 SCCI 原始字节，Core 1 按队列回放，遇到 0x80 用硬件定时器精确延时。

## 架构

```
Core 0 (主循环)                    Core 1 (播放引擎)
UART 接收字节                      ┌─────────────────┐
  ↓                                │ 读队列           │
SCCI 握手? → RS/OK                 │ 0x80? → 精确延时 │
  ↓ 否                             │ 其他? → SCCI 解析 │
ring_write(buf, byte)              │   → PIO 写寄存器  │
  ↓                                └─────────────────┘
LED + WS2812                       硬件定时器驱动 tick
```

- Core 0：UART 接收 → 环形缓冲区（生产者）+ LED/WS2812
- Core 1：环形缓冲区（消费者）→ SCCI 解析 + 0x80 精确延时 + PIO 写寄存器

## 环形缓冲区

```c
// src/ring_buf.h
#define RING_SIZE 16384  // 16KB

typedef struct {
    uint8_t buf[RING_SIZE];
    volatile uint16_t head;  // Core 0 写位置
    volatile uint16_t tail;  // Core 1 读位置
} ring_buf_t;

// 单写单读，volatile 保证 Core 间可见性
static inline bool ring_write(ring_buf_t *r, uint8_t byte);
static inline bool ring_read(ring_buf_t *r, uint8_t *byte);
static inline uint16_t ring_used(ring_buf_t *r);
static inline uint16_t ring_free(ring_buf_t *r);
static inline uint8_t ring_peek(ring_buf_t *r);
```

## SCCI 播放引擎（Core 1）

Core 1 运行完整的 SCCI 协议解析状态机（和当前 main loop 一样），
但从环形缓冲区读而不是从 UART 读。遇到 0x80 时用硬件定时器精确延时一个 tick。

```c
// Core 1 主循环
void core1_main(void) {
    uint8_t parse_idx = 0;
    uint8_t slot, cmd, addr;

    while (true) {
        uint8_t byte;
        while (!ring_read(&s_ring, &byte)) {
            tight_loop_contents();
        }

        if (parse_idx == 0) {
            if (byte == 0x80) {
                busy_wait_us_32(TICK_US);  // 精确延时 23µs
            } else if (byte == 0xFF) {
                // 握手不在这里处理，Core 0 负责
            } else if ((byte & 0xF0) == 0x00) {
                slot = byte & 0x0F;
                parse_idx = 1;
            }
        }
        // ... 其余 SCCI 解析同当前逻辑
    }
}
```

## Tick 时长

DMPlayer 上位机 `spfm_lite.c` 确认：
- `spfm_hw_wait(samples)`: 每个 sample 塞一个 0x80
- `us = wait_samples * 1000000 / 44100`
- **一个 0x80 = 一个 VGM sample = 1/44100 秒 ≈ 22.68µs**
- `HW_WAIT_THRESHOLD = 10`：wait < 10 samples 走 0x80 硬件延时，>= 10 走上位机本地 sleep

RPFM 遇到 0x80 应延时 **22.68µs ≈ 23µs**。

## Core 0 职责

保留现有功能 + 新增环形缓冲区管理：

1. UART 接收（`getchar_timeout_us(0)`）
2. 握手处理（0xFF → RS, 0xFE → OK + IC# reset）— 仍在 Core 0
3. 非 0x80/非握手字节 → `ring_write()`
4. 0x80 也写入环形缓冲区（Core 1 负责延时）
5. LED 心跳 + WS2812 指示
6. 缓冲区水位上报（可选）

## 流控

Core 0 检查 `ring_free()`，如果缓冲区 >75% 满，可通过 UART 告知上位机暂停。
简单实现：水位超过阈值时设置标志，上位机定期查询。

## 文件修改

### 新增
- `src/ring_buf.h` — 环形缓冲区 inline 实现

### 修改
- `src/main.c`:
  - Core 0: UART 接收 → ring_write + 握手 + LED
  - Core 1: `multicore_launch_core1(core1_main)` 播放 SCCI
  - SCCI 解析状态机从 main loop 移到 Core 1
  - `#include "pico/multicore.h"`
- `CMakeLists.txt`: 添加 `pico_multicore`

### 不修改
- `src/ym2413.pio`
- `src/pio_cs.pio`
- `src/ws2812.pio`

## 实施步骤

1. 新建 `src/ring_buf.h` — 环形缓冲区
2. 修改 `src/main.c`:
   - 添加 `#include "pico/multicore.h"` 和 `ring_buf.h`
   - 定义 `ring_buf_t s_ring`
   - Core 0: 简化 main loop（UART → ring_write + 握手 + LED）
   - Core 1: `core1_main()` — SCCI 解析 + 0x80 延时 + PIO 写寄存器
   - `multicore_launch_core1(core1_main)` 在 init 末尾
3. 修改 `CMakeLists.txt`: target_link_libraries 加 `pico_multicore`
4. 编译测试

## 验证

1. 编译成功
2. SCCI 握手正常（0xFF → RS, 0xFE → OK）
3. YM2413 播放正常，音质不低于当前版本
4. AY8910 大数据量 VGM 播放速度稳定
5. WS2812 LED 正常
