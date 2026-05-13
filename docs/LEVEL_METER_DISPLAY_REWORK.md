# AY8910 音量条/电平表显示逻辑改造

## 状态：已完成

## Context

原始问题：
- ch0-2 (Tone A/B/C) 音量条只在 toneOn=true 时显示，noise only、env mode、DAC mode 都不显示
- ch3 (Noise) 和 ch4 (Env) 下方文字显示的是 N+period / E+shape，没有反映实际音量
- 需要统一显示：ch0-2 始终反映真实音量寄存器值，ch3/4 借用关联方波通道音量

## 改动文件

`RPFM_Player/src/ay8910_window.cpp`

## 核心规则

### ch0-2 (方波通道 A/B/C)

**音量条**：直接反映音量寄存器值 `vol & 0x0F`，线性映射 `v / 15.0f`。
- v > 0 时：音量条 = 真实音量
- v = 0 时：快衰减 (×0.85)，营造视觉释放感
- 不受 T/E/N 模式影响

**下方文字**：
- DAC 模式 (tone+noise 全关, vol > 0, 非 ENV)：显示 "DAC:vol"
- 其他情况：显示真实音量数字 (0-15)

### ch3 (Noise 通道)

**音量条**：借用 noiseOn 的方波通道中最大音量值，无衰减。
- 任何方波通道 noiseOn=true → 取 `(vol[j] & 0x0F) / 15.0f` 最大值
- 全部 noiseOn=false → 音量条为 0

**下方文字**：显示 noiseOn 通道中的最大音量数字。

### ch4 (Envelope 通道)

**音量条**：借用 ENV 模式 (bit4=1) 的方波通道中最大音量值。
- 有 ENV 通道且 maxVol > 0 → `maxVol / 15.0f`
- 有 ENV 通道但全部 vol=0 → 满幅度 (1.0f)
- 无 ENV 通道 → 音量条为 0

**下方文字**：
- ENV 通道全部 vol=0 → 显示 "ENV"
- 有 vol > 0 的 ENV 通道 → 显示最大音量数字
- 无 ENV 通道 → 显示 "0"

### 钢琴键盘

- vol > 0 → 按真实音量亮起
- vol = 0 但 ENV 模式开启 (bit4=1) → 满幅度亮起，显示音高
- 纯真实音量驱动，不衰减

## 代码关键修改

### UpdateToneChannelLevels

```cpp
// 音量条：真实音量 + 快衰减
float rawLv = v / 15.0f;
if (v > 0) {
    chDecay[i] = rawLv;
} else {
    chDecay[i] *= 0.85f;
    if (chDecay[i] < 0.01f) chDecay[i] = 0.0f;
}
channelLevel[chIdx] = chDecay[i];

// 钢琴键盘：纯真实音量，ENV 模式 vol=0 时满幅度
float pianoLv = (v > 0) ? rawLv : (useEnv ? 1.0f : 0.0f);
```

### UpdateNoiseChannelLevel

```cpp
// 直接取 noiseOn 通道最大音量，无衰减
channelLevel[chIdx] = anyNoiseOn ? maxNVol : 0.0f;
```

### UpdateEnvChannelLevel

```cpp
// ENV 通道：借用最大音量，全部 vol=0 则满幅度
envLv = (maxEVol > 0) ? maxEVol : 1.0f;
channelLevel[chIdx] = envLv;
```

### RenderLevelMeters 文字显示

```cpp
// ch0-2: DAC 模式显示 "DAC:vol"，否则显示数字
// ch3: noiseOn 通道最大音量数字
// ch4: ENV 通道 vol 全为 0 则 "ENV"，否则最大音量数字
```

## AY8910 音量寄存器说明

| bit4 | bits 0-3 | 含义 |
|------|----------|------|
| 0 | 0-15 | 固定音量模式，值 = 实际音量 |
| 1 | 0-15 | 包络模式，bits 0-3 为包络选择，实际幅度由包络发生器控制 |

ENV 模式下 bits 0-3 不是真实音量，而是包络形状选择。
但对于 UI 可视化，ch0-2 仍然显示 vol & 0x0F 的值，因为这反映了寄存器写入的原始数据。
