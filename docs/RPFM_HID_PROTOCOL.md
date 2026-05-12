# RPFM HID 协议实现记录

## 背景

SCCI 版本（CDC）在大数据量 VGM 播放时 USB CDC 溢出导致死机。根本原因：CDC 无背压/流控，上位机灌数据速度 > 下位机处理速度 → 丢字节 → 帧错 → 死机。

解决方案：自定义 RPFM 协议，USB HID 64B 帧通信，天然流控。

## 协议帧格式

```
下行帧 (上位机→下位机, 64 bytes):
  [0]     CMD      命令字节
  [1]     SEQ      序列号 (0-255, 回卷)
  [2]     LEN      有效载荷长度 (0-60)
  [3..62] PAYLOAD  数据
  [63]    CRC8     校验 (多项式 0x31)

上行帧 (下位机→上位机, 64 bytes):
  [0]     ACK      = SEQ (确认收到)
  [1]     STATUS   状态位 (bit0=播放中, bit1=缓冲区>75%, bit2=错误)
  [2..3]  BUF_LVL  缓冲区水位 (uint16, bytes)
  [4..63] 保留

CMD 列表:
  0x01  WRITE_REG   写寄存器: [slot, addr, data] × N
  0x02  WRITE_TICK  延时 tick
  0x03  RESET       复位: [slot_mask]
  0x04  VGM_DATA    VGM 命令流
  0x05  VGM_START   开始播放
  0x06  VGM_STOP    停止播放
  0x20  BOOTSEL     进入 BOOTSEL 模式
  0xFF  NOP         空操作
```

## 关键技术发现

### 1. TinyUSB HID Set_Report buffer 偏移

**问题**：`HidD_SetOutputReport()` 发送 65 字节（1 report ID + 64 数据），TinyUSB `tud_hid_set_report_cb()` 收到的 buffer 是否包含 report ID？

**结论**：TinyUSB 源码 `hid_device.c:337`：
```c
if ((report_id != HID_REPORT_TYPE_INVALID) && (report_len > 1) && (report_id == report_buf[0])) {
    report_buf++;  // 跳过 report ID
    report_len--;
}
```
`HID_REPORT_TYPE_INVALID = 0`。当 `HidD_SetOutputReport` 传 report_id=0 时，条件 `0 != 0` 为 false，**不跳过**。

但 Windows HID 驱动在 `HidD_SetOutputReport` 时会把 buffer[0]（report ID）放到 USB 控制传输的 `wValue` 里，实际发送的数据从 buffer[1] 开始。所以 **TinyUSB 收到的 buffer 就是不含 report ID 的纯数据**，`buffer[0]` = CMD。

**经验**：不要假设 TinyUSB 会/不会跳过 report ID，用 raw buffer echo 验证实际收到的数据。

### 2. Deferred Write Queue — HID 回调不能直接写 PIO

**问题**：从 `tud_hid_set_report_cb()` 直接调用 `write_reg()` 写 PIO，AY8910 只有咔声。

**根因分析**：
- SCCI 版本在主循环的 UART 轮询里直接调 `ym2413_write_reg_raw()`，工作正常
- HID 回调在 `tud_task()` 内部执行，可能处于 USB 中断上下文
- PIO SM 的 FIFO 操作在中断上下文中行为可能不同

**解决方案**：Deferred write queue
- HID 回调只入队：`deferred_push(slot, addr, data)`
- 主循环消费：`deferred_pop()` → `write_reg()`
- 与 SCCI 版本的"主循环轮询 UART → 直接写"模式一致

### 3. AY8910 写入时序

**MegaGRRL 参考** (`driver.c`):
- `Driver_FmOutpsg()`: PSG(AY8910) 每步 `Driver_Sleep(1)` = **1µs**
- `Driver_FmOutopll()`: YM2413(OPLL) 每步 `Driver_Sleep(20)` = **20µs**

**RPFM 实现**：AY8910 和 YM2413 共用 `ym2413_pio_write_reg()`，使用 20µs 时序（保守值，兼容所有芯片）。实际验证 AY8910 在 20µs 时序下工作正常。

### 4. AY8910 寄存器表

| Reg | 名称 | 说明 |
|-----|------|------|
| 0x00 | Ch A tone fine | 低8位 |
| 0x01 | Ch A tone coarse | 高4位 |
| 0x02-0x05 | Ch B/C tone | 同上 |
| 0x06 | Noise period | |
| 0x07 | Mixer | bit=1禁用, bit0-2=tone, bit3-5=noise |
| 0x08-0x0A | Ch A/B/C volume | bit0-3音量, bit4=envelope |
| 0x0B-0x0C | Envelope period | |
| 0x0D | Envelope shape | |

Tone period = clock / (16 * freq)

**测试音**：2MHz 时钟，C4(261Hz)，period=479 (fine=223, coarse=1)
- Reg 0x07 = 0x3E（开 Ch A tone）
- Reg 0x08 = 0x0F（Ch A 最大音量）
- Reg 0x00 = 223（fine）
- Reg 0x01 = 1（coarse）

参考：`reference/AY-3-8910-master/AY-3-8910_test.ino`

### 5. HID 描述符 — 无 OUT endpoint

USB HID 描述符只有 IN endpoint (0x81)，没有 OUT endpoint。Windows `WriteFile()` 会失败，必须使用 `HidD_SetOutputReport()` (Set_Report 控制传输)。

上位机通信方式：
- 发送：`HidD_SetOutputReport(h, buf, 65)` — buf[0]=report ID, buf[1..64]=frame
- 接收：`HidD_GetInputReport(h, buf, 65)` — buf[0]=report ID(需预填), 返回 buf[1..64]=frame

### 6. TinyUSB 初始化

Pico SDK 的 TinyUSB 初始化方式：
```c
tud_init(BOARD_TUD_RHPORT);  // 不是 tusb_init()
```

必须在 `tusb_config.h` 中定义：
```c
#define BOARD_TUD_RHPORT       0
#define BOARD_TUD_MAX_SPEED    OPT_MODE_DEFAULT_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64
```

不要使用 `pico_enable_stdio_usb`，否则 CDC 和自定义 HID 冲突。

### 7. TinyUSB 配置文件覆盖

Pico SDK 自带的 `tusb_config.h` 和项目自定义的冲突。解决方法：
```cmake
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})
```
`BEFORE` 确保项目目录优先于 SDK 目录。

## 自动烧录流程

`scripts/flash_rpfm.sh`:
1. `hid_test.exe bootsel` — 通过 HID 发送 CMD_BOOTSEL，RP2350A 进入 BOOTSEL 模式
2. 等待 RP2350 U 盘出现（PowerShell 检测 Volume label 匹配 "RP"）
3. `cp rpfm.uf2 DRIVE:/` — 复制 UF2，设备自动重启

避免手动按 BOOTSEL 按钮和拔插 USB。

## 文件结构

```
src/rpfm/
  main.c              — 入口 + HID 回调 + deferred queue + 主循环
  usb_descriptors.c   — HID 设备描述符 (VID=0x2E8A, PID=0x1090)
  tusb_config.h       — TinyUSB 配置（HID only，无 CDC）
  protocol.h          — 帧格式 + CRC8
  command_buffer.h    — 32KB 环形缓冲区（Phase 2 VGM 用）
  ym2413.pio          — PIO 14-bit 并行总线驱动
  pio_cs.pio          — PIO 4-bit CS 输出
  ws2812.pio          — WS2812 LED 驱动

test/
  hid_test.c          — Windows HID 调试工具 (gcc -lhid -lsetupapi)

scripts/
  flash_rpfm.sh       — 自动烧录脚本
```

## 开发时间线

```
2026.05.12  HID 协议固件框架（Phase 1）
2026.05.12  HID 枚举成功，上位机 Python hidapi 测试通过
2026.05.12  编写 Windows HID 调试工具 hid_test.exe（无依赖）
2026.05.12  BOOTSEL 命令 + 自动烧录脚本
2026.05.12  发现 HID 回调直接写 PIO 无声音 → deferred queue
2026.05.12  发现上位机 mute 太快导致只有咔声 → 5 秒播放验证
2026.05.13  AY8910 HID 写入验证通过，C4 持续方波 5 秒
2026.05.13  清理调试代码，对标 SCCI 版本结构
```

## 待办

- [ ] Phase 2：VGM 命令解析引擎（双核：Core 0 HID + Core 1 播放）
- [ ] Phase 3：MSC Flash 文件管理（拖放 VGM）
- [ ] Phase 4：DMPlayer AY8910 窗口改造（SCCI → RPFM HID）
