# RPFM Player GUI 可视化设计

## 概述

RPFM Player 的 AY8910 可视化系统包含三个核心组件：电平表、钢琴键盘、寄存器表。
所有可视化的数据源是影子寄存器（shadow registers），通过 tick 同步机制与固件播放位置精确对齐。

## 数据流

```
VGM 文件解析 → {reg, data, tick} 入队 (ShadowRegDelayQueue)
                       ↓
固件 HID 响应 → fwTick (44100 Hz sample position)
                       ↓
flushTo(fwTick) → apply 影子寄存器 → s_vol[], s_toneOn[], s_noiseOn[] 等
                       ↓
UpdateChannelLevels() → channelLevel[], pianoKeyOn[]
                       ↓
RenderLevelMeters() / RenderPianoKeyboard() → ImGui 绘制
```

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

## 四、Tick 同步机制

影子寄存器更新通过 tick-based 延迟队列同步，确保可视化与固件播放位置对齐。

详见 [BUFFER_SIZE_SLIDER_AND_SHADOW_REG_SYNC.md](BUFFER_SIZE_SLIDER_AND_SHADOW_REG_SYNC.md)。

核心流程：
1. VGM 解析器解析每个命令时记录当前 tick
2. 寄存器写入 {reg, data, tick} 入队
3. 每帧从固件 HID 响应获取 fwTick
4. flushTo(fwTick)：只 apply tick ≤ fwTick 的更新
5. 可视化读取更新后的影子寄存器

## 五、侧边栏配置

| 设置 | 说明 |
|------|------|
| PIO Delay | AY8910 /WR 脉冲宽度 (0-2000ns) |
| Buffer Size | VGM 缓冲区 (64B - 2KB) |
| Playback Mode | Live / Buffered |
| Clock Correction | AY8910 时钟微调 |
| Tick Display | 固件 tick / 总 tick + 时间 |
| Portamento | 滑音显示开关 |
| Microtonal | 微音程显示开关 |

所有设置持久化到 `ay8910_config.ini`。

## 六、示波器 (Scope)

可配置高度的波形显示区域，每个通道独立波形。
数据源：基于通道 tone period 模拟的方波/噪声波形。

## 七、双芯片支持

AY8910 支持双芯片模式（CS0 + CS1），所有可视化组件自动扩展到 10 通道：
- ch0-4: 芯片 0 (A, B, C, Noise, Env)
- ch5-9: 芯片 1 (A, B, C, Noise, Env)

两组独立的影子寄存器，独立的 tick 同步。
