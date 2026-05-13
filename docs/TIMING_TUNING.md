# AY8910 PIO 写入时序调整

## 背景

AY8910 通过 14-bit 并行总线写入寄存器，时序由 PIO + 软件 delay 控制。
不同的 VGM 数据密度对时序要求不同：密集鼓声数据需要快速写入，否则会拖慢播放；
太快的时序可能导致芯片锁存错误（缺通道）。

## 时序参数

### PIO Write Delay（侧边栏滑块）

- **范围**: 0 - 2000 ns
- **默认值**: 1000 ns (1µs)
- **精度**: 100ns 步进
- **协议**: `CMD_SET_DELAY` (0x21)，payload 1 字节（0-200，单位 100ns）

### 时序路径

```
上位机滑块 → CMD_SET_DELAY → s_ay_delay_100ns (volatile)
Core 1 VGM loop → ay8910_pio_write_reg() → ay_delay_100ns()
```

### AY8910 写入序列（7 个 PIO word）

```
word 1: addr on bus, A0=0, /WR=high  (address setup)
word 2: addr on bus, A0=0, /WR=low   (address latch)  ← delay 后
word 3: addr on bus, A0=0, /WR=high  (address hold)
word 4: data on bus, A0=1, /WR=high  (data setup)
word 5: data on bus, A0=1, /WR=low   (data latch)     ← delay 后
word 6: data on bus, A0=1, /WR=high  (data hold)
word 7: idle (0x03FF)
```

delay 只在 /WR 下降沿后插入（word 2→3 和 word 5→6 之间）。

## 历史调整记录

### 500ns
- 最初尝试的快速时序
- 缺通道，芯片锁存不稳定

### 1µs (1000ns)
- 参考 MegaGRRL SSG 时序
- 通道完整，但 Core 0 timer ISR 下仍有降调

### PIO clkdiv 方案
- 尝试用 PIO SM 时钟分频控制时序
- ISR 非阻塞推 FIFO，但累积延迟仍在
- 放弃：调整 delay 仍影响音高

### Core 1 方案（当前）✅
- VGM 播放搬到 Core 1，MegaGRRL-style cycle counter
- Core 1 专用 tight loop，不受 USB 影响
- delay 调整不再影响播放速度
- 单击即播放，之前多次点击的 bug 也解决

## 参考时序

| 来源 | /WR 脉冲宽度 | 备注 |
|------|-------------|------|
| AY-3-8910 datasheet | 典型 500ns | 最小值 |
| MegaGRRL SSG | ~1µs | 经验证全速播放 SNDH |
| RPFM 默认 | 1000ns | 上位机可调 |
| RPFM 最小测试 | 200ns | 有声音但不稳定 |

## 相关文件

- `src/rpfm/spfm_bus.pio` — `ay8910_pio_write_reg()`, `ay_delay_100ns()`
- `src/rpfm/vgm_player.h` — Core 1 VGM 主循环
- `src/rpfm/main.c` — `s_ay_delay_100ns` 全局变量, `CMD_SET_DELAY` 处理
- `src/rpfm/protocol.h` — `CMD_SET_DELAY` 命令定义
- `RPFM_Player/src/ay8910_window.cpp` — 侧边栏 ns 滑块
- `RPFM_Player/src/rpfm_hid.cpp` — `rpfm_set_ay_delay()`
