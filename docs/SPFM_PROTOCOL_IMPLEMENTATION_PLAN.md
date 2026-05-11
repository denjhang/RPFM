# RPFM SPFM 协议兼容实施计划（修订版）

## Context

面包板 YM2413 PIO 驱动已验证可用。下一步实现 SPFM 协议兼容，让 RPFM 通过 USB CDC 接收上位机发送的 SPFM 协议数据，实时驱动音频芯片。

参考源码：`20240827_stm32f103_SPFM_DIY/HARDWARE/YMF288/YMF288M.c`

## 1. SPFM 总线时序分析

### 1.1 MegaGRRL 精确时序（参考源：driver.c）

所有时序基于 ESP32 240MHz，`Driver_Sleep(N)` = N µs，`Driver_Output()` = 移位寄存器输出（~几 µs）。

#### Driver_FmOutopll (YM2413 OPLL) — 两相位，每步 20µs
```
A0=0 → sleep(20)
CS# low + D=Register → sleep(20)
WR# low → sleep(20) → WR# high → sleep(20)
A0=1 + D=Value + WR# low → sleep(20)
WR# high + CS# high → sleep(20)
总计: ~120µs
```

#### Driver_FmOutopn (YM2203 OPN) — 两相位，中间 10µs
```
A0=0 + CS# low + D=Register + WR# low → out → WR# high → out → sleep(10)
A0=1 + D=Value + WR# low → out → WR# high + CS# high → out
后续: reg<=0x0f: sleep(1), else: sleep(20)
总计: ~30µs (不含后续延迟)
```

#### Driver_FmOutopna (YM2608 OPNA) — 两相位 + A1 选择
```
设 A1 → A0=0 + CS# low + D=Register + WR# low → out → WR# high → out → sleep(10)
A0=1 + D=Value + WR# low → out → WR# high + CS# high → out
总计: ~20µs (不含后续延迟)
```

#### Driver_FmOutopl3 (YM2612 OPL3) — 两相位 + A1 选择，每步 20µs
```
设 A1 → A0=0 → sleep(20)
CS# low + D=Register → sleep(20)
WR# low → sleep(20) → WR# high → sleep(20)
A0=1 + D=Value + WR# low → sleep(20)
WR# high + CS# high → sleep(20)
总计: ~120µs
```

#### Driver_FmOutpsg (AY-3-8910) — 完全不同的两步 CS# 操作
```
A0=0 + CS# low + D=Register → sleep(1) → CS# high → sleep(1)
D=Value → sleep(1)
A0=1 + CS# low → sleep(1) → CS# high → sleep(1)
总计: ~4µs, 无 WR# 信号！
```

### 1.2 STM32 SPFM 原版时序（YMF288M.c）

#### Chips_BusWrite — FM 芯片
```
CS# low, WR# high → 设 A0-A3 → delay_us(1) → 写 D0-D7 → WR# low → ~10 NOP → WR# high + CS# high → NOP x2 → D0-D7=0xFF
WR# 低电平: ~140ns
```

#### Chips_BusWriteSN — SN76489
```
CS# low, A0-A3 low, WR# high → 写 D0-D7 → WR# low → 3 NOP → delay_us(18) → WR# high + CS# high → NOP x3 → D0-D7=0xFF
WR# 低电平: ~18µs
```

### 1.3 时序差异总结

| 芯片 | A0 相位 | WR# 低电平 | CS# 操作 | 地址间延迟 |
|------|---------|-----------|---------|-----------|
| YM2413 OPLL | 2 相 | ~150ns | 整次操作 | 20µs |
| YM2151 OPM | 1 相 | ~150ns | 整次操作 | ~20µs |
| YM2203 OPN | 2 相 | ~150ns | 整次操作 | 10µs |
| YM2608 OPNA | 2 相+A1 | ~150ns | 整次操作 | 10µs |
| YM2612 OPL3 | 2 相+A1 | ~150ns | 整次操作 | 20µs |
| SN76489 | 无 | 18µs | 整次操作 | 无 |
| AY-3-8910 | 2 相 | 无 WR# | CS# 两步脉冲 | 1µs |

### 1.4 PIO 设计方案

所有 FM 芯片共享同一 WR# 脉冲时序（~150ns），区别仅在 A0/A1 延迟和 CS# 控制。
SN76489 的 18µs WR# 延迟由 C 侧 `busy_wait_us_32(18)` 控制。
AY-3-8910 无 WR# 信号，只有 CS# 脉冲。

**一个 PIO 程序** `spfm_bus_write`，处理 D0-D7 输出 + WR# 脉冲。
不同芯片的时序差异在 C 侧用 `busy_wait_us_32()` 控制。
这样灵活性最高，不需要为每个芯片写单独的 PIO 程序。

## 2. SPFM 协议解析（来自 Chips_Process，YMF288M.c）

### 2.1 单字节命令（param_count=0 时处理）

| 数据 | 含义 | 响应 |
|------|------|------|
| 0xFF | 握手 | 回复 "RS" |
| 0xFE | 复位 | Chips_Reset() + 回复 "OK" |
| 0x80 | NOP | 不做操作 |
| 0x81 | 停止 | LED=1 |
| 0x00-0x0F | 选择 slot | ucSlotNum=data, param_count++ |

### 2.2 第二字节（param_count=1）：命令+地址

| 数据范围 | 命令类型 | ucBoardAddress | 说明 |
|----------|----------|----------------|------|
| 0x0N | 连续写 FM | N=data&0x0F | 4 字节，A0=0 然后 A0=1 |
| 0x8N | 单次写 FM | N=data&0x0F | 3 字节，只写 A0=0 |
| 0x2N | SN76489 | - | 6 字节，4 次BusWriteSN |
| 0x4N | 保留 | - | 空实现 |
| 0x3N/0x5N-0xFN | 保留 | - | 空实现，param_count 不变 |

### 2.3 第三字节（param_count=2）：第一个数据

| 命令 | 操作 | param_count |
|------|------|-------------|
| 0x0_ | BusWrite(slot, boardAddr, data) — A0=0 | → 3 |
| 0x8_ | BusWrite(slot, boardAddr, data) — A0=0 | → 0 (完成) |
| 0x2_ | BusWriteSN(slot, data) + SN C0 修复 | → 3 |
| 其他 | 空操作（switch 留空） | 不变 |

### 2.4 第四字节及后续（param_count≥3）

| 命令 | 操作 | param_count |
|------|------|-------------|
| 0x0_ | BusWrite(slot, boardAddr\|0x01, data) — A0=1 | → 0 (完成) |
| 0x2_ | 继续接收，直到 param_count=5 | param_count<5: ++, ≥5: →0 |

### 2.5 总线调用映射

```
0x0_ (连续写FM，4字节): BusWrite(slot, addr, data) [A0=0] + BusWrite(slot, addr|1, data) [A0=1]
0x8_ (单次写FM，3字节): BusWrite(slot, addr, data) [A0=0] 只写一次
0x2_ (SN76489，6字节):  BusWriteSN(slot, data) x4 次，含 C0 修复
```

### 2.6 SN76489 C0 修复逻辑

```c
if (snFix == 1) {
    if (data >= 0xc0 && data <= 0xc1) data = 0xc1;
}
else if ((data & 0x07) == 0x07) snFix = 1;
else snFix = 0;
```

### 2.7 命令长度汇总

| 命令 | 字节数 | 总线操作 |
|------|--------|---------|
| 0xFF | 1 | 回复 RS |
| 0xFE | 1 | 复位 + 回复 OK |
| 0x80/0x81 | 1 | 无/NOP |
| 0x0N + data + data | 4 | BusWrite x2 (FM 连续写) |
| 0x8N + data | 3 | BusWrite x1 (FM 单次写) |
| 0x2N + data x4 | 6 | BusWriteSN x4 (SN76489) |
| 0x4N | 3+ | 保留 |

## 3. RPFM 实现方案

### 3.1 文件结构

```
RPFM/
├── main.c              → 修改：SPFM 协议主循环 + 模式切换
├── spfm_bus.c/h        → 新建：底层总线驱动（调用 PIO）
├── spfm_protocol.c/h   → 新建：协议状态机
├── spfm_fm.pio         → 新建：FM 芯片 PIO 驱动（YM2413/YM2151/YM2608）
├── spfm_sn.pio         → 新建：SN76489 PIO 驱动
├── ym2413.pio          → 保留（兼容旧 demo 模式）
├── flash.sh            → 不变
├── CMakeLists.txt      → 修改：添加新文件和 PIO 生成
```

### 3.2 PIO 程序 — 按芯片类型拆分

#### spfm_fm.pio — FM 芯片总线驱动

复用当前 ym2413.pio 的 pack_phase 逻辑，WR# 快速脉冲。

```pio
.program spfm_fm_write
    pull block
    out pins, 9     ; setup: D=data, WR#=1 (bit8=1)
    nop [5]         ; setup hold: 6 cycles @125MHz = 48ns
    out pins, 9     ; WR# low (bit8=0)
    nop [5]         ; WR# low: 48ns
    out pins, 9     ; WR# high (bit8=1)
    nop [5]         ; data hold: 48ns

% c-sdk {
// init + pack_phase + pio_write_phase 同 ym2413.pio
%}
```

C 侧 `spfm_bus_write_fm()` 负责 A0-A3 + CS# 控制和 busy_wait 延迟。

#### spfm_sn.pio — SN76489 总线驱动

WR# 低电平保持 18µs，由 PIO 内部循环实现。

```pio
.program spfm_sn_write
    pull block
    out pins, 9     ; setup: D=data, WR#=1
    ; WR# pulse: 需要 18µs @125MHz = 2250 cycles
    ; 用循环实现: 10 次外循环 x 225 内循环
    .initial wr_on
wr_on:
    out pins, 9     ; WR# low
    set x, 10       ; 外循环 10 次
outer:
    set y, 224      ; 内循环 225 次 (224+1)
inner:
    nop [31]        ; 32 cycles
    jmp y--, inner  ; 1 cycle → 225 * 33 = 7425 cycles/外循环
    jmp x--, outer  ; 10 * 7425 = 74250 cycles = 594µs -- 太长
```

18µs = 2250 cycles 太长不适合纯 PIO 循环。**改用 PIO + C 侧 busy_wait**：

```pio
.program spfm_sn_write
    pull block
    out pins, 9     ; setup: D=data, WR#=1
    out pins, 9     ; WR# low (bit8=0)
    ; C 侧 busy_wait_us_32(18) 后再写一次释放 WR#
```

实际上 SN76489 最简单的实现：
- PIO 输出 data + WR#=low
- C 侧等 18µs
- PIO 输出 data + WR#=high

#### spfm_bus.c 中的 FM vs SN 调用

```c
// FM 写入：使用 spfm_fm_write SM
void spfm_bus_write_fm(slot, addr, data) {
    set_A0_A3(addr);
    CS_low(slot);
    pio_sm_put_blocking(fm_pio, fm_sm, pack_phase_fm(data));
    while (!pio_sm_is_tx_fifo_empty(fm_pio, fm_sm)) {}
    CS_high(slot);
}

// SN76489 写入：使用 spfm_sn_write SM
void spfm_bus_write_sn(slot, data) {
    A0_A3_low();
    CS_low(slot);
    pio_sm_put_blocking(sn_pio, sn_sm, pack_phase_sn(data, WR_LOW));
    busy_wait_us_32(18);
    pio_sm_put_blocking(sn_pio, sn_sm, pack_phase_sn(data, WR_HIGH));
    CS_high(slot);
}
```

### 3.3 spfm_bus.c — 总线驱动

```c
// 外部声明（来自 spfm.pio 的 C SDK 块）
extern PIO s_pio;
extern uint s_sm;

void spfm_bus_init(void);
void spfm_bus_write_fm(uint8_t slot, uint8_t address, uint8_t data);
void spfm_bus_write_sn(uint8_t slot, uint8_t data);
void spfm_bus_reset(void);
```

### 3.4 spfm_protocol.c — 协议状态机

直接移植 `Chips_Process` 逻辑，每个字节调用 `spfm_protocol_feed(data)`。

### 3.5 main.c 改造

```
上电 → LED 闪 3 次 → 演示曲
         ↓
收到 0xFF 握手 → 停止演示 → 进入 SPFM 模式
         ↓
SPFM 主循环：getchar → spfm_protocol_feed → 总线写入
         ↓
收到 "BOOTSEL\n" → 进入烧录模式
```

### 3.6 引脚分配

```c
#define PIN_BUS_BASE  0     // GPIO0-8 = D0-D7 + WR# (PIO)
#define PIN_A0   9          // 地址线 0
#define PIN_CS0  10         // 片选 0 (slot 0)
#define PIN_IC   11         // 复位
#define PIN_CS1  12         // 片选 1 (slot 1)
#define PIN_CS2  13         // 片选 2 (slot 2)
#define PIN_CS3  14         // 片选 3 (slot 3)
#define PIN_A1   15         // 地址线 1（YM2413 不用，预留给 YM2151）
#define PIN_A2   16         // 地址线 2
#define PIN_A3   17         // 地址线 3
#define PIN_LED  25
```

### 3.7 串口缓冲

USB CDC 的 TinyUSB 内部已有 64 字节 FIFO。对于大数据量 VGM 流，需要在协议层做环形缓冲：

```c
#define SPFM_RX_BUF_SIZE 4096  // 4KB 环形缓冲

static uint8_t s_rx_buf[SPFM_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;

// 在主循环中批量读取 CDC 数据到缓冲
void spfm_rx_fill(void) {
    while (ring_has_space()) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) break;
        ring_put(ch);
    }
}

// 协议处理从缓冲取数据
void spfm_protocol_poll(void) {
    spfm_rx_fill();
    while (ring_has_data()) {
        spfm_protocol_feed(ring_get());
    }
}
```

## 4. 实施步骤

### Step 1: 创建 spfm.pio
- 重命名 PIO 程序为通用的 `spfm_bus_write`（不再叫 ym2413）
- C SDK 块提供 `pack_phase()` 和 `pio_bus_write()` 辅助函数

### Step 2: 创建 spfm_bus.c/h
- `spfm_bus_init()` — GPIO 初始化
- `spfm_bus_write_fm()` — FM 芯片写入（A0-A3 + CS# + PIO WR# 脉冲）
- `spfm_bus_write_sn()` — SN76489 写入（CS# + PIO WR# + 18µs 延迟）
- `spfm_bus_reset()` — IC# 复位

### Step 3: 创建 spfm_protocol.c/h
- 移植 `Chips_Process` 状态机
- 添加环形缓冲
- `spfm_protocol_feed()` — 逐字节解析
- `spfm_protocol_poll()` — 从缓冲取数据并处理

### Step 4: 修改 main.c
- 上电演示模式（保留当前 demo）
- 收到 0xFF 切换到 SPFM 模式
- SPFM 模式主循环
- BOOTSEL 命令保留

### Step 5: 更新 CMakeLists.txt
- 添加 `spfm_bus.c`、`spfm_protocol.c`
- `pico_generate_pio_header` 添加 `spfm.pio`

### Step 6: 编译烧录测试

## 5. 关键文件

| 文件 | 路径 | 操作 |
|------|------|------|
| YMF288M.c | Reference_Project/20240827_stm32f103_SPFM_DIY/HARDWARE/YMF288/ | 参考源 |
| YMF288M.h | 同上 | 引脚定义参考 |
| spfm.pio | RPFM/ | 新建 |
| spfm_bus.c/h | RPFM/ | 新建 |
| spfm_protocol.c/h | RPFM/ | 新建 |
| main.c | RPFM/ | 修改 |
| CMakeLists.txt | RPFM/ | 修改 |

## 6. 验证

1. `bash build.sh` 编译无错误
2. `bash flash.sh` 烧录成功
3. 上电演示曲正常播放（DEMO 模式）
4. Denjhang Music Player 连接 COM 口发送 0xFF，收到 "RS"
5. 上位机播放 YM2413 VGM，扬声器实时播放音乐
6. BOOTSEL 命令仍可用
