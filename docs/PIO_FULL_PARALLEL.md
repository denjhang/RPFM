# PIO 全并行总线升级记录

> 日期：2026-05-12
> Commit: 5542f4d

## 背景

之前 YM2413 总线驱动采用混合模式：
- PIO 控制：D0-D7 + WR#（GPIO0-8，9 个连续引脚）
- CPU 控制：A0（GPIO9）、CS0#（GPIO10）通过 `gpio_put()` 切换

问题：USB CDC 中断可能在 `gpio_put(A0)` 和 `pio_write_phase()` 之间触发，导致 A0/CS0# 与数据总线不同步，产生时序抖动。

## 改动

将 A0、CS0# 纳入 PIO 输出，所有 11 个总线信号原子切换。

### 引脚分配（不变）

```
GPIO0-7:   D0-D7   bit[7:0]
GPIO8:     WR#     bit[8]   (1=空闲, 0=写脉冲)
GPIO9:     A0      bit[9]   (0=地址, 1=数据)
GPIO10:    CS0#    bit[10]  (1=未选, 0=选中)
```

### PIO 程序（ym2413.pio）

```pio
.program ym2413_write
    pull block
    out pins, 11
```

极简 2 条指令，`sm_config_set_wrap` 循环执行 pull-out。C 侧控制时序。

### 11-bit 打包

```c
uint32_t pack_bus(uint8_t data, bool a0, bool wr_n, bool cs0_n) {
    return ((cs0_n ? 1u : 0u) << 10)
         | ((a0 ? 1u : 0u) << 9)
         | ((wr_n ? 1u : 0u) << 8)
         | data;
}
```

### 写寄存器时序

每次 `ym2413_pio_write_reg(reg, data)` push 7 个 word：

```
地址相位 (A0=0, CS0#=0):
  setup:  pack(reg, A0=0, WR#=1, CS0#=0)  → wait 20µs
  pulse:  pack(reg, A0=0, WR#=0, CS0#=0)  → wait 20µs
  hold:   pack(reg, A0=0, WR#=1, CS0#=0)  → wait 20µs

数据相位 (A0=1, CS0#=0):
  setup:  pack(data, A0=1, WR#=1, CS0#=0) → wait 20µs
  pulse:  pack(data, A0=1, WR#=0, CS0#=0) → wait 20µs
  hold:   pack(data, A0=1, WR#=1, CS0#=0) → wait 20µs

释放:
  release: pack(0xFF, A0=0, WR#=1, CS0#=1)
```

## 删除的代码

- `ym2413_write_reg_bitbang()` — CPU bitbang 驱动
- `s_use_pio` 开关变量
- CPU 对 A0/CS0# 的 `gpio_init`/`gpio_set_dir`/`gpio_put` 调用

## 踩坑

| 问题 | 原因 | 解决 |
|------|------|------|
| 烧录后死机，LED 不亮 | PIO 没设 `sm_config_set_wrap`，执行完 out 后 PC 越界 | 加 wrap 回 offset~offset+1 |
| 烧录后死机（第二次） | `pio_sm_set_pins` 在 `pio_sm_init` 前被覆盖，引脚状态不确定 | 用 `pio_sm_set_pins_with_mask` 绝对地址 + mask |

## 改进

1. **时序原子性**：D0-D7 + WR# + A0 + CS0# 在同一个 PIO cycle 输出，不受中断影响
2. **代码简化**：删除 bitbang fallback 和 CPU 总线控制代码，减少约 70 行
3. **扩展性**：pack_bus 函数直接支持多片选扩展（改 CS# bit 即可）
