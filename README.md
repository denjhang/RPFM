# RPFM — RP2350A Sound Chip Player

RPFM 是基于 RP2350A 的复古音源芯片硬件播放器，是 SPFM (Sound Player for Multiple chips) 系列的继任者。通过 PIO 精确时序驱动 YM2413、AY8910 等经典音源芯片，实现硬件级 VGM 播放。

![RPFM Overview](pics/overview.png)

## 特性

- **双核 VGM 播放**：Core 0 USB HID 通信，Core 1 MegaGRRL-style cycle counter 精确定时（44100Hz sample-accurate）
- **PIO 并行总线**：14-bit 并行总线直接驱动音源芯片，软件可控纳秒级写入时序
- **Tick 同步可视化**：固件回报播放 tick，上位机影子寄存器精确同步钢琴键盘/电平表/音高
- **实时/缓冲双模式**：Live 模式逐寄存器写入，Buffered 模式流式传输 VGM 原始数据
- **USB HID 64B 帧协议**：天然流控，CRC8 校验，避免 CDC 溢出死机
- **HID 一键烧录**：通过 HID BOOTSEL 命令自动进入烧录模式，无需按按钮
- **多芯片支持**：片选独立，支持 YM2413 (OPLL) / AY8910 (PSG)，未来扩展 YM2151/YM2612 等

## 硬件架构

```
PC (上位机) ──USB HID──→ RP2350A (下位机) ──PIO──→ 音源芯片 → 模拟音频
                              │
                         ┌────┴────┐
                         │ Core 0  │  USB HID callback, tud_task(), LED, deferred writes
                         │ Core 1  │  VGM tight loop, cycle counter, PIO register writes
                         └─────────┘
```

### GPIO 接线

| GPIO | 功能 | 说明 |
|------|------|------|
| GPIO0-7 | D0-D7 | 8-bit 数据总线 |
| GPIO8 | WR# | 写信号 (active low) |
| GPIO9 | RD# | 读信号 (active low) |
| GPIO10-13 | A0-A3 | 地址线 |
| GPIO16 | WS2812 | 板载 RGB LED (片选指示) |
| GPIO17-20 | CS0-CS3 | 4-bit 片选 |
| GPIO21 | IC# | 全局复位 |
| GPIO25 | LED | 心跳指示 |

### 电平转换

RP2350A 3.3V GPIO 通过 74LVC245 电平转换到 5V，驱动 YM2413 等传统 5V 芯片。

## 通信协议

USB HID 64 字节帧协议：

```
下行帧 (PC → RP2350A):
  [0]  CMD      命令字节
  [1]  SEQ      序列号 (0-255)
  [2]  LEN      载荷长度 (0-60)
  [3..62] PAYLOAD
  [63] CRC8     校验 (多项式 0x31)

上行帧 (RP2350A → PC):
  [0]  ACK      = SEQ
  [1]  STATUS   bit0=播放中, bit1=缓冲区>75%, bit2=错误
  [2..3] BUF_LVL  缓冲区水位 (uint16 LE)
  [4..7] TICK     当前播放 tick (uint32 LE, 44100Hz)
```

### 命令列表

| CMD | 名称 | 说明 |
|-----|------|------|
| 0x01 | WRITE_REG | YM2413 寄存器写入 (20µs 时序) |
| 0x08 | WRITE_AY | AY8910 寄存器写入 (可调时序) |
| 0x04 | VGM_DATA | VGM 原始字节流 |
| 0x05 | VGM_START | 开始 VGM 播放 |
| 0x06 | VGM_STOP | 停止 VGM 播放 |
| 0x21 | SET_DELAY | 设置 AY8910 /WR 脉冲宽度 (100ns 单位) |
| 0x20 | BOOTSEL | 进入 BOOTSEL 烧录模式 |
| 0x03 | RESET | 硬件复位 (IC#) |

详见 [docs/RPFM_HID_PROTOCOL.md](docs/RPFM_HID_PROTOCOL.md)。

## VGM 播放引擎

### Core 1 精确定时

参考 MegaGRRL (ESP32) 方案，Core 1 运行 tight loop：

```c
// Core 1 VGM 主循环
while (true) {
    if (!(s_status & STATUS_PLAYING)) continue;

    uint64_t cc = timer_hw->timerawl;   // 64-bit µs 计数器
    cycle_us += (cc - last_cc);
    uint64_t sample = cycle_us * 44100ULL / 1000000ULL;

    if (sample >= next_sample) {
        // 解析 VGM 命令，写 PIO，推进 tick
        process_vgm_commands();
        s_vgm_tick = next_sample;  // 回报给上位机
    }
}
```

- `timer_hw->timerawl` 是硬件 64-bit µs 计数器，双核可读，不受中断影响
- Core 1 专用 tight loop，不受 USB HID 中断抢占
- AY8910 PIO 写入阻塞不影响 Core 0 的 USB 通信

### Tick 同步机制

固件每次处理完 VGM 命令后更新 `s_vgm_tick`（当前 sample 位置），通过 HID 响应回报给上位机。上位机解析 VGM 字节流时同步追踪 sample tick，将影子寄存器更新入队，只 apply tick ≤ 固件回报 tick 的更新。

```
上位机解析 VGM → {reg, data, tick} 入队
                       ↓
固件 HID 响应 → fwTick
                       ↓
flushTo(fwTick) → apply 影子寄存器 → UI 可视化
```

详见 [docs/BUFFER_SIZE_SLIDER_AND_SHADOW_REG_SYNC.md](docs/BUFFER_SIZE_SLIDER_AND_SHADOW_REG_SYNC.md)。

### AY8910 PIO 写入时序

7 个 PIO word 完成一次寄存器写入，软件 delay 控制时序：

```
word 1: addr on bus, A0=0, /WR=high  (address setup)
word 2: addr on bus, A0=0, /WR=low   (address latch)  ← delay
word 3: addr on bus, A0=0, /WR=high  (address hold)
word 4: data on bus, A0=1, /WR=high  (data setup)
word 5: data on bus, A0=1, /WR=low   (data latch)     ← delay
word 6: data on bus, A0=1, /WR=high  (data hold)
word 7: idle (0x03FF)
```

默认 1µs /WR 脉冲，可通过上位机滑块实时调整 (0-2000ns)。详见 [docs/TIMING_TUNING.md](docs/TIMING_TUNING.md)。

## 上位机 (RPFM Player)

Windows 桌面播放器，基于 Dear ImGui，支持 AY8910 实时/缓冲双模式播放。

### 功能

- **钢琴键盘**：实时显示当前音符，多通道彩色按键，滑音/颤音指示
- **电平表**：每通道独立电平条 + dB 刻度，点击通道按钮 mute/solo
- **寄存器表**：实时显示影子寄存器，tone/noise/mixer/volume/envelope 全可视化
- **示波器**：可配置波形显示
- **播放控制**：VGM 文件加载、播放/暂停/停止、进度条、循环
- **侧边栏配置**：PIO 延时调整、缓冲区大小 (64B-2KB)、播放模式切换、时钟校正等
- **Tick 显示**：固件播放位置 / 总 tick，时间同步
- **配置持久化**：所有侧边栏设置保存到 `ay8910_config.ini`

### 构建

```
cd RPFM_Player
mkdir build2 && cd build2
cmake ..
cmake --build . --config Release
```

输出：`bin/rpfm_player.exe`

## 固件构建与烧录

### 依赖

- Pico SDK 2.2.0（项目内含）
- ARM GCC cross-compiler (`arm-none-eabi-gcc`)
- CMake + Make

### 构建

```bash
mkdir build_rpfm && cd build_rpfm
cmake ..
cmake --build .
```

输出：`rpfm.uf2`

### 烧录

```bash
# 自动烧录（HID BOOTSEL）
bash scripts/flash_rpfm.sh

# 手动：按住 BOOTSEL 按钮插 USB，复制 UF2 到 RPI-RP2 盘
cp build_rpfm/rpfm.uf2 /path/to/RPI-RP2/
```

## 项目结构

```
RPFM/
├── src/rpfm/               # 固件源码
│   ├── main.c              # 入口、HID 回调、deferred queue、主循环
│   ├── vgm_player.h        # Core 1 VGM 播放引擎 (cycle counter)
│   ├── spfm_bus.pio        # PIO 14-bit 并行总线驱动
│   ├── pio_cs.pio          # PIO 4-bit 片选输出
│   ├── ws2812.pio          # PIO WS2812 LED 驱动
│   ├── protocol.h          # HID 帧格式 + CRC8
│   └── command_buffer.h    # 环形缓冲区
├── RPFM_Player/             # Windows 上位机
│   ├── src/
│   │   ├── ay8910_window.cpp  # AY8910 主窗口 (UI + VGM 播放)
│   │   ├── rpfm_hid.cpp/h     # HID 通信层
│   │   └── rpfm_protocol.h    # 协议常量
│   └── bin/                   # 构建输出
├── scripts/                 # 构建/烧录脚本
│   ├── flash_rpfm.sh        # HID 自动烧录
│   └── build.sh             # 构建脚本
├── docs/                    # 设计文档
│   ├── ARCHITECTURE.md      # 系统架构
│   ├── CORE1_VGM_PLAYER_PLAN.md  # Core 1 播放器设计
│   ├── RPFM_HID_PROTOCOL.md      # HID 协议详解
│   ├── TIMING_TUNING.md          # 时序调整记录
│   └── BUFFER_SIZE_SLIDER_AND_SHADOW_REG_SYNC.md  # Tick 同步
├── pics/                    # 图片资源
│   └── overview.png
└── reference/               # 参考代码 (MegaGRRL, AY-3-8910 等)
```

## 芯片支持状态

| 芯片 | 状态 | 说明 |
|------|------|------|
| YM2413 (OPLL) | ✅ 已验证 | 20µs PIO 写入时序 |
| AY8910 (PSG) | ✅ 已验证 | 可调 PIO 写入时序，Core 1 VGM 播放 |
| YM2151 (OPM) | 🔜 计划中 | 8 通道 FM |
| YM2612 (OPN2) | 🔜 计划中 | 6 通道 FM + DAC |
| YM2203 (OPN) | 🔜 计划中 | 3 FM + 3 SSG |
| YM2608 (OPNA) | 🔜 计划中 | 6 FM + 3 SSG + ADPCM |

## 开发历史

RPFM 是 IAP-RESPFM (2022, STM32F103) → DIY SPFM (2024, STM32) → RP2350A (2026) 的第三代。

- **2026.05.12** — 项目启动，PIO 并行总线驱动 YM2413/AY8910
- **2026.05.12** — USB HID 64B 帧协议替代 CDC，解决溢出死机
- **2026.05.12** — HID 自动烧录 (BOOTSEL 命令)
- **2026.05.13** — Core 1 VGM 播放引擎 (MegaGRRL-style cycle counter)
- **2026.05.13** — 上位机 DMPlayer AY8910 窗口改造 (SCCI → RPFM HID)
- **2026.05.13** — Tick 同步影子寄存器，缓冲区 64B-2KB 滑块

## License

MIT
