# RPFM 全 PIO 总线驱动 + WS2812 LED 实施计划

## Context

参考 MegaGRRL 移位寄存器架构，所有总线信号（D0-D7 + WR# + A0-A3 + CS0-CS3）都由 PIO 原子输出，避免 CPU 中断导致时序抖动。WS2812 LED 做状态指示。

## 引脚分配

PIO 连续控制 17 个引脚（GPIO0-16）：

```
GPIO0-7:   D0-D7      (PIO bit 0-7)
GPIO8:     WR#        (PIO bit 8)
GPIO9:     A0         (PIO bit 9)
GPIO10:    A1         (PIO bit 10)
GPIO11:    A2         (PIO bit 11)
GPIO12:    A3         (PIO bit 12)
GPIO13:    CS0#       (PIO bit 13)
GPIO14:    CS1#       (PIO bit 14)
GPIO15:    CS2#       (PIO bit 15)
GPIO16:    CS3#       (PIO bit 16)
GPIO17:    WS2812     (PIO1 SM)
GPIO18:    IC# (复位)  (CPU)
GPIO25:    LED 心跳    (CPU)
```

PIO 17-bit word 格式：
- bits[7:0]   = D0-D7
- bit[8]      = WR# (1=idle, 0=pulse)
- bits[12:9]  = A0-A3
- bits[16:13] = CS0#-CS3# (1=idle, 0=selected)

## PIO 程序

### spfm_fm.pio — FM 写入（17 位）

3-phase 输出：setup(WR#=1) → pulse(WR#=0) → hold(WR#=1)

PIO 每次 pull 输出 17 位，C 侧连续 push 3 次：

```pio
.program spfm_fm_write
    pull block
    out pins, 17     ; D0-D7 + WR# + A0-A3 + CS0-CS3
```

C 侧 pack 函数：
```c
static inline uint32_t pack_bus(uint8_t data, uint8_t addr, uint8_t slot, bool wr_high) {
    uint32_t val = data;                                    // bits[7:0] = D0-D7
    val |= (wr_high ? 1u : 0u) << 8;                       // bit[8] = WR#
    val |= (uint32_t)(addr & 0x0F) << 9;                   // bits[12:9] = A0-A3
    uint32_t cs = 0x0Fu << 13;                              // bits[16:13] all CS high
    if (slot < 4) cs &= ~(1u << (13 + slot));               // selected CS low
    val |= cs;
    return val;
}
```

FM 写入流程：
1. Push setup（D=data, A=addr, CS=selected, WR#=1）
2. Push pulse（D=data, A=addr, CS=selected, WR#=0）
3. Push hold（D=data, A=addr, CS=selected, WR#=1）
4. Push release（D=0xFF, A=0, CS=all high, WR#=1）

### spfm_sn.pio — SN76489（17 位）

两阶段：WR#=low → C 侧 busy_wait_us_32(18) → WR#=high

```pio
.program spfm_sn_write
    pull block
    out pins, 17
```

SN 写入流程：
1. Push data + CS=selected + A0-A3=0 + WR#=0
2. busy_wait_us_32(18)
3. Push data + CS=selected + A0-A3=0 + WR#=1
4. Push release（all CS high）

### spfm_bus.c 修改

- `spfm_bus_write_fm(slot, addr, data)`: 4 次 PIO push（setup/pulse/hold/release）
- `spfm_bus_write_sn(slot, data)`: 3 次 PIO push + 18µs wait
- `spfm_bus_reset()`: CPU 控制 IC#（GPIO18），PIO 输出 idle 状态
- `spfm_bus_init()`: 初始化 PIO0 SM0(FM) + SM1(SN) + 17 个 GPIO

## WS2812 LED（4颗，GPIO17）

使用 pico-sdk 自带 ws2812.pio，PIO1 SM0。

| LED | 用途 |
|-----|------|
| 0 | RX 指示（收到数据闪绿） |
| 1 | TX 指示（发送 RS/OK 闪蓝） |
| 2 | CS0 片选（CS0 有效时亮） |
| 3 | CS1 片选（CS1 有效时亮） |

## PIO 资源

| PIO | SM | 用途 |
|-----|-----|------|
| PIO0 SM0 | FM 总线 (17-bit) |
| PIO0 SM1 | SN76489 总线 (17-bit) |
| PIO1 SM0 | WS2812 LED |

## 实施步骤

1. 重写 `spfm_fm.pio` — 17 位 out pins
2. 重写 `spfm_sn.pio` — 17 位 out pins
3. 重写 `spfm_bus.c` — 新引脚 + 17 位 pack + CS PIO 控制
4. 新建 `ws2812_led.c/h`
5. 修改 `spfm_protocol.c` — RX/TX LED 指示
6. 修改 `main.c` — 初始化 + LED 心跳
7. 修改 `CMakeLists.txt`

## 验证

1. 编译成功
2. GPIO25 心跳闪烁
3. WS2812 LED 正常
4. SCCI 握手 0xFF→RS, 0xFE→OK
5. FM 写入时序正确
