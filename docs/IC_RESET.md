# IC# 复位与芯片初始化

> 日期：2026-05-12
> 硬件：RP2350A 核心板 + RESPFM Bus Board v0.7

## 1. IC# 是全局复位，不是芯片复位

IC#（Initialize Control）是 SPFM 总线的**全局复位信号**，拉低时所有卡槽上的芯片同时复位。不是针对某个芯片的复位。

```
IC# ───→ 所有 CS0-CS3 的芯片同时复位
CS# ───→ 片选，决定数据送给哪个芯片
```

RPFM 函数命名从 `ym2413_reset()` 改为 `bus_ic_reset()`，避免误解。

## 2. 参考固件的复位方式

### 二代 IAP（STC15，2 卡槽）

上电时 IC# 从低开始，延时几百 ms 后拉高。**不写任何寄存器，不选 CS。**

```c
// main() 初始化
BUS_DATA_PORT = 0;
BUS_WR = 1;
BUS_RD = 1;
BUS_BSEL0 = 1;  // CS 全高（未选中）
BUS_BSEL1 = 1;
BUS_IC = 0;      // IC# 低

UART_Init();
for(i=0; i<32767; i++);  // 延时几百ms
for(i=0; i<32767; i++);

BUS_IC = 1;      // IC# 高，复位完成
```

收到 0xFE（握手信号2）时再次 IC# 复位：

```c
BUS_IC = 0;
for(i=0; i<20000; i++);  // ~几ms
BUS_IC = 1;
for(i=0; i<20000; i++);
UART_TransmitByte('O');
UART_TransmitByte('K');
```

### 三代 IAP-RESPFM（STC15，4 卡槽）

与二代相同，上电时 `BUS_IC = 0; BUS_IC = 1;`（连续两句，几乎无延时）。
收到 0xFE 时 IC# 低 → 延时 → IC# 高 → 回 OK。

**不写任何寄存器，不选 CS。所有芯片初始化由上位机负责。**

### STM32 DIY（STM32F103，4 卡槽）

唯一做了芯片级初始化的参考固件，但也只针对 **SN76489** 和 **YM2608**：

```c
void Chips_Reset() {
    // CS 全低 + WR 高 + A 全低 + D 输出
    BUS_PORT->BRR = (BUS_BSEL0|BUS_BSEL1|BUS_BSEL2|BUS_BSEL3);
    BUS_PORT->BSRR = BUS_WR;
    BUS_PORT->BRR = BUS_A0|BUS_A1|BUS_A2|BUS_A3;

    GPIO_WriteBit(BUS_PORT, BUS_IC, 0);
    delay_ms(100);                        // IC# 低 100ms
    GPIO_WriteBit(BUS_PORT, BUS_IC, 1);

    BUS_PORT->BSRR = BUS_WR|BUS_BSEL0|...; // 释放 CS + WR
    Chips_WriteData(0x00);                 // D=0x00（不是 0xFF！）
    delay_ms(100);

    // SN76489 初始化（静音所有通道）
    for(i=0; i<4; i++) {
        Chips_BusWriteSN(i, 0x9F);
        Chips_BusWriteSN(i, 0xBF);
        Chips_BusWriteSN(i, 0xDF);
        Chips_BusWriteSN(i, 0xFF);
    }

    // YM2608 初始化
    for(i=0; i<4; i++) {
        Chips_BusWrite(i, 0, 0x29);
        Chips_BusWrite(i, 1, 0x80);
        // ...
    }
}
```

注意：STM32 DIY 的 `Chips_Reset()` 里 **没有 YM2413 初始化**。

## 3. 对比总结

| 项目 | 上电复位 | 0xFE 复位 | 芯片初始化 |
|------|---------|----------|-----------|
| 二代 IAP | IC# 低→延时→高 | IC# 低→延时→高 | 无（上位机负责） |
| 三代 IAP-RESPFM | IC# 低→高 | IC# 低→延时→高 | 无（上位机负责） |
| STM32 DIY | IC# 低 100ms→高 | `Chips_Reset()` 完整版 | SN76489 + YM2608（无 YM2413） |
| **RPFM（当前）** | **cs_select(0) + IC# 低 50ms→高** | **IC# 低 50ms→高** | **无（上位机负责）** |

## 4. 踩坑记录

### IC# 低电平保持时间

| 尝试 | IC# 低时间 | 结果 |
|------|-----------|------|
| 1ms | 1ms | 无声 |
| 50ms | 50ms | 有声 |

YM2413 数据手册要求 IC# 低电平保持一定时间（tRC）。1ms 不够，50ms 可以。

### mute_all 的误导

最初 `ym2413_mute_all()` 写 19 个寄存器（9 通道关闭 + 9 音量静音 + 节奏关闭），加上 IC# 复位后有声音。
删除 mute_all 后无声，误以为需要写寄存器才能激活芯片。

**实际原因：** 能发声的版本在 mute_all 之前有 `cs_select(0)`，CS0 被选中。删除 mute_all 时同时删除了 `cs_select(0)`，
PIO 写入的数据到达不了 YM2413。mute_all 本身不是必需的——参考固件都不做。

### 关键发现：cs_select 必须在复位前

```c
// 错误：CS 没选中，IC# 复位后 YM2413 收不到任何数据
bus_ic_reset();

// 正确：先选中 CS0，IC# 复位后 YM2413 能接收上位机的寄存器写入
cs_select(0);
bus_ic_reset();
```

但参考固件复位时 CS 都是高的（不选中）。为什么参考可以而 RPFM 不行？

**可能原因：** 参考固件是 CPU GPIO 直出，每次 BusWrite 都会选 CS 再释放。
RPFM 用 PIO 驱动总线，CS 由独立的 PIO1 控制，如果 CS 一直是高（未选中），
上位机发来的 SCCI 数据包里的 CS 选择信息需要正确传递到 PIO1 CS 驱动。

### 当前方案

RPFM 当前在 init 时 `cs_select(0)` 后不再释放。后续 SCCI 协议解析中的 CS 切换
由上位机控制。这与参考固件的 `BusWrite()` 每次 select/deselect 不同，
但因为 YM2413 只在 WR# 低时采样数据，CS 一直低不会造成问题。

## 5. 待验证

- [ ] 不做 cs_select(0)，只靠上位机 SCCI 协议选 CS，是否有声音
- [ ] IC# 低 20ms 是否够（在 50ms 和 1ms 之间二分查找最小值）
- [ ] 多芯片时 cs_select 的管理策略（选中后才写，写完释放）
