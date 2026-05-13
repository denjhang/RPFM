# RPFM Player GUI 可视化设计

## 概述

RPFM Player 的 AY8910 可视化系统包含三个核心组件：电平表、钢琴键盘、寄存器表。
所有可视化的数据源是影子寄存器（shadow registers），两种播放模式各有独立的数据通路。

## 播放模式与数据流

### Live 模式

VGMPlaybackThread 按 44100Hz 真实速度解析 VGM，每次 0xA0 命令同时发 HID 写入 + 更新影子寄存器。GUI 线程每帧读影子寄存器渲染。

```
VGMPlaybackThread (NORMAL priority)
    ↓ VGMProcessCommand() per 1ms timer tick
    ↓ 0xA0 → UpdateAY8910State() + ay8910_write_reg() + safe_flush()
    ↓ 影子寄存器即时更新
    ↓
UpdateChannelLevels() → channelLevel[], pianoKeyOn[]
    ↓
RenderLevelMeters() / RenderPianoKeyboard()
```

### 缓冲模式（独立可视化线程）

**架构**：可视化线程完全独立于流线程，自己打开 VGM 文件本地副本，按 44100Hz 速度独立解析，只更新影子寄存器不发 HID。流线程只负责向固件发送数据。GUI 线程每帧读影子寄存器渲染。

```
VGMStreamThread (BELOW_NORMAL)          VGMVisualizationThread (NORMAL)
    ↓ HID 发送 VGM 数据                    ↓ QPC + 1ms multimedia timer
    ↓ s_fwTick 更新 (进度条用)              ↓ VizProcessCommand() 解析 VGM
    ↓                                      ↓ 只调 UpdateAY8910State()，不发 HID
    ↓                                      ↓ atomic_thread_fence(release)
    ↓                                      ↓
    ↓              GUI thread ←────────────┘
    ↓              UpdateChannelLevels()
    ↓              atomic_thread_fence(acquire)
    ↓              RenderLevelMeters() / RenderPianoKeyboard()
```

**为什么需要独立线程**：缓冲模式下数据通过 HID 发给固件，上位机只有固件回报的 tick（受 USB 延迟影响）。如果可视化依赖 HID 数据流，更新频率会被 USB 帧率限制（~1ms/帧），导致 GUI 更新一卡一卡。独立线程让可视化以纯 CPU 速度运行，不受 I/O 影响。

**跨线程内存可见性**：Release 编译下编译器/CPU 可能将影子寄存器的读取缓存到寄存器中，导致 GUI 线程看不到可视化线程的写入。通过 `std::atomic_thread_fence` 确保：
- 可视化线程：写入影子寄存器后执行 `memory_order_release`
- GUI 线程：读取影子寄存器前执行 `memory_order_acquire`

## 一、电平表 (Level Meters)

5 通道（双芯片模式 10 通道）：A, B, C, Noise, Env。

### 音量条

每个通道的音量条高度由 `channelLevel[ch]` 决定 (0.0 ~ 1.0)，颜色渐变：
- 低电平：蓝色
- 中电平：绿色
- 高电平：黄色 → 红色

#### ch0-2 (方波通道)

音量条始终反映真实音量寄存器值 `(vol & 0x0F) / 15.0f`：
- vol > 0：直接设置，无平滑
- vol = 0：快衰减 (×0.85/帧)，营造视觉释放感

不受 tone/noise/env 模式影响。

#### ch3 (Noise 通道)

借用 3 个方波通道中 noiseOn=true 的通道的最大音量值。无衰减。

#### ch4 (Envelope 通道)

借用 3 个方波通道中 ENV 模式 (bit4=1) 的通道的最大音量值。
- 有 ENV 通道且 vol > 0：最大音量值
- 有 ENV 通道但全部 vol=0：满幅度 (1.0f)
- 无 ENV 通道：0

### 下方文字

| 通道 | 条件 | 显示 |
|------|------|------|
| ch0-2 | DAC 模式 (tone+noise off, vol>0, 非ENV) | "DAC:vol" |
| ch0-2 | 正常 | vol 数字 (0-15) |
| ch3 | 有 noiseOn 通道 | 最大 vol 数字 |
| ch3 | 无 noiseOn 通道 | "0" |
| ch4 | ENV 通道全部 vol=0 | "ENV" |
| ch4 | 有 vol>0 的 ENV 通道 | 最大 vol 数字 |
| ch4 | 无 ENV 通道 | "0" |

### 通道按钮交互

- 左键：Mute/Unmute 单通道
- 右键：Solo（只听该通道）
- 滚轮：Invert all（全部反相）

Mute 状态：红色按钮 + 音量条打 X。
Solo 状态：黄色边框。

## 二、钢琴键盘 (Piano Keyboard)

范围 C1 (MIDI 24) ~ B7 (MIDI 107)，84 键。

### 按键亮起条件

- 方波通道 vol > 0：按真实音量亮起
- 方波通道 vol = 0 但 ENV 模式开启 (bit4=1)：满幅度亮起
- 纯真实音量驱动，不经过衰减

### 音高来源

方波通道的 tone period 寄存器 (coarse << 8 | fine)，转换公式：

```
freq = ay8910_clock / (16.0 * period)
midi_note = round(69 + 12 * log2(freq / 440.0))
```

### 多通道叠加

同一琴键可被多个通道同时按下，取最大音量。
每个按键记录所有按下的通道，用于颜色混合。

### 滑音/颤音指示

当 `s_showPortamento` 开启时，连续追踪频率变化：
- key-on edge 设置 anchor 频率
- 每帧计算 semitone offset：`12 * log2(freq / anchor)`
- 动态变化显示蓝色滑条，静态微音程需要手动开启 `s_showMicrotonal`

### 通道颜色

每个通道独立颜色，琴键颜色取最后按下的通道色。

## 三、寄存器表 (Register Table)

实时显示影子寄存器值：

| 寄存器 | 内容 |
|--------|------|
| 0x00-0x05 | Tone A/B/C period (fine + coarse) |
| 0x06 | Noise period |
| 0x07 | Mixer (tone/noise enable) |
| 0x08-0x0A | Volume A/B/C (bit4 = ENV mode) |
| 0x0B-0x0C | Envelope period (fine + coarse) |
| 0x0D | Envelope shape |

双芯片模式显示两组寄存器。

## 四、侧边栏配置

| 设置 | 说明 |
|------|------|
| PIO Delay | AY8910 /WR 脉冲宽度 (0-2000ns) |
| Buffer Size | VGM 缓冲区 (512B - 8KB) |
| Playback Mode | Live / Buffered |
| Clock Correction | AY8910 时钟微调 |
| Tick Display | 固件 tick / 总 tick + 时间 |
| Portamento | 滑音显示开关 |
| Microtonal | 微音程显示开关 |

所有设置持久化到 `ay8910_config.ini`。

## 五、示波器 (Scope)

可配置高度的波形显示区域，每个通道独立波形。
数据源：基于通道 tone period 模拟的方波/噪声波形。

## 六、双芯片支持

AY8910 支持双芯片模式（CS0 + CS1），所有可视化组件自动扩展到 10 通道：
- ch0-4: 芯片 0 (A, B, C, Noise, Env)
- ch5-9: 芯片 1 (A, B, C, Noise, Env)

两组独立的影子寄存器，独立的可视化线程读取。
