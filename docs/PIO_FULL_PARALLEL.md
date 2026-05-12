# PIO 全并行总线升级记录

> 日期：2026-05-12
> Commit: 5542f4d (11-bit), 待 commit (14-bit)

## 背景

之前 YM2413 总线驱动采用混合模式：
- PIO 控制：D0-D7 + WR#（GPIO0-8，9 个连续引脚）
- CPU 控制：A0（GPIO9）、CS0#（GPIO10）通过 `gpio_put()` 切换

问题：USB CDC 中断可能在 `gpio_put(A0)` 和 `pio_write_phase()` 之间触发，导致 A0/CS0# 与数据总线不同步，产生时序抖动。

## 第一次升级：11-bit（GPIO0-10）

将 A0、CS0# 纳入 PIO 输出，所有 11 个总线信号原子切换。

```
GPIO0-7:   D0-D7   bit[7:0]
GPIO8:     WR#     bit[8]   (1=空闲, 0=写脉冲)
GPIO9:     A0      bit[9]   (0=地址, 1=数据)
GPIO10:    CS0#    bit[10]  (1=未选, 0=选中)
```

## 第二次升级：14-bit 总线 + 4-bit CS（GPIO0-13 + GPIO17-20）

为支持多芯片（YM2413/YM2151/YM2608/YM2612 等），补全完整 SPFM 总线信号：
- 新增 RD#（读信号）、A1-A3（地址线）
- CS0-CS3 从 PIO0 分离到 PIO1 SM1，独立控制

### PIO0 SM0: 14-bit 数据+控制总线（GPIO0-13）

```
GPIO0-7:   D0-D7   bit[7:0]
GPIO8:     WR#     bit[8]   (1=空闲, 0=写脉冲)
GPIO9:     RD#     bit[9]   (1=空闲, 0=读)
GPIO10:    A0      bit[10]  (地址线0)
GPIO11:    A1      bit[11]  (地址线1)
GPIO12:    A2      bit[12]  (地址线2)
GPIO13:    A3      bit[13]  (地址线3)
```

空闲状态: `0x03FF` (D=0xFF, WR#=1, RD#=1, A0-A3=0)

14-bit 打包：
```c
uint32_t pack_bus(uint8_t data, uint8_t addr, bool wr_n, bool rd_n) {
    return ((uint32_t)(wr_n ? 1u : 0u) << 8)
         | ((uint32_t)(rd_n ? 1u : 0u) << 9)
         | ((uint32_t)(addr & 0x0F) << 10)
         | (uint32_t)data;
}
```

### PIO1 SM1: 4-bit 片选（GPIO17-20）

```
GPIO17:    CS0#    bit[0]   (1=空闲, 0=选中)
GPIO18:    CS1#    bit[1]
GPIO19:    CS2#    bit[2]
GPIO20:    CS3#    bit[3]
```

空闲状态: `0x0F`。选择芯片 N: `0x0F & ~(1 << N)`

CS 独立于 PIO0 数据总线，CS 切换和数据写入各自 push 到不同 PIO。

### CPU 控制

```
GPIO21:    IC#     复位（上电复位一次，无时序要求）
GPIO25:    LED     心跳
```

### 写寄存器时序

每次 `ym2413_pio_write_reg(reg, data)` push 7 个 word 到 PIO0（CS 由 PIO1 单独保持选中状态）：

```
地址相位 (A0=0):
  setup:  pack(reg, 0, WR#=1, RD#=1)  → wait 20µs
  pulse:  pack(reg, 0, WR#=0, RD#=1)  → wait 20µs
  hold:   pack(reg, 0, WR#=1, RD#=1)  → wait 20µs

数据相位 (A0=1):
  setup:  pack(data, 1, WR#=1, RD#=1) → wait 20µs
  pulse:  pack(data, 1, WR#=0, RD#=1) → wait 20µs
  hold:   pack(data, 1, WR#=1, RD#=1) → wait 20µs

释放:
  pack(0xFF, 0, WR#=1, RD#=1) = BUS_IDLE (0x03FF)
```

### 改动文件

| 文件 | 改动 |
|------|------|
| `src/ym2413.pio` | 11-bit → 14-bit: `out pins, 14`，新 pack_bus，移除 CS0# |
| `src/pio_cs.pio` | 新建: PIO1 SM1 CS 驱动 (`pull block; out pins, 4`) |
| `src/main.c` | 新引脚 PIN_IC=21, PIO1 CS 初始化, 移除旧 A1-A3 CPU 初始化 |
| `CMakeLists.txt` | 新增 `pico_generate_pio_header` pio_cs.pio |

## 踩坑

| 问题 | 原因 | 解决 |
|------|------|------|
| 烧录后死机，LED 不亮 | PIO 没设 `sm_config_set_wrap`，执行完 out 后 PC 越界 | 加 wrap 回 offset~offset+1 |
| 烧录后死机（第二次） | `pio_sm_set_pins` 在 `pio_sm_init` 前被覆盖，引脚状态不确定 | 用 `pio_sm_set_pins_with_mask` 绝对地址 + mask |
| `pio_instr_mem_write` 链接失败 | RP2350 (pio v3) API 变化，该函数不存在 | CS PIO 改用 `.pio` 文件 + `pio_add_program` |
