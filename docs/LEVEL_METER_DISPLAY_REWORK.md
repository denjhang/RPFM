# AY8910 音量条/电平表显示逻辑改造

## 状态：计划中

## Context

当前 ch0-2 (Tone A/B/C) 的音量条只在 toneOn=true 时显示，noise only、env mode、DAC mode 都不显示。
ch3 (Noise) 和 ch4 (Env) 下方文字显示的是 N+period / E+shape，没有反映实际音量。
需要改为更直观的显示：ch0-2 统一音量条，ch3/4 显示关联通道的音量信息。

## 改动文件

`RPFM_Player/src/ay8910_window.cpp`

## 一、ch0-2 (Tone A/B/C) 音量条逻辑改造

### 当前逻辑 (UpdateToneChannelLevels, line ~1044)

- `chDecay[i]` 只在 `toneOn=true && vol>0` 时维持，否则快速衰减
- 音量条只响应 tone 开启状态

### 改为

ch0-2 只要 volume > 0 就显示音量条，不管 tone/noise/env 状态。

新增状态标记：

```cpp
static bool s_chEnvOnly[3] = {};  // tone+noise both off but env mode on
static bool s_chDacMode[3] = {};  // tone+noise both off but vol > 0
```

key-on 触发条件改为：

```cpp
bool new_on = (vol[i] & 0x1F) > 0;  // 不再要求 toneOn
```

`chDecay[i]` 衰减逻辑：

```cpp
bool anyOn = ton || noi;
if (new_on && anyOn) {
    chDecay[i] = 1.0f;          // sustain
} else if (new_on && !anyOn) {
    chDecay[i] *= 0.95f;        // DAC/ENV-only: medium decay
} else {
    chDecay[i] *= 0.85f;        // fast decay
}
```

Level calculation：

```cpp
if (useEnv && !anyOn) {
    lv = chDecay[i];            // ENV-only: full amplitude
} else if (useEnv) {
    lv = chDecay[i];
} else {
    lv = (v >= 15) ? 0.0f : (1.0f - (float)v / 15.0f) * chDecay[i];
}
```

### ch0-2 下方文字显示

```
if (toneOn || noiseOn) && !(vol & 0x10):
    显示 volume 数字 (0-15)
if (vol & 0x10) && (toneOn || noiseOn):
    显示 "ENV"
if !(toneOn || noiseOn) && vol > 0:
    显示 "DAC:vol"（音量条反映真实音量）
```

## 二、ch3 (Noise) 文字显示

显示 3 个方波通道中 noiseOn=true 的通道的 volume 最大值：

```cpp
uint8_t maxVol = 0;
for (int j = 0; j < 3; j++) {
    if (noiseOn[j]) {
        uint8_t v = vol[j] & 0x0F;
        if (v > maxVol) maxVol = v;
    }
}
snprintf(volStr, sizeof(volStr), "%d", maxVol);
```

## 三、ch4 (Env) 文字显示

显示 3 个方波通道中 env mode (bit4=1) 的通道的 volume 最大值：

```cpp
uint8_t maxVol = 0;
bool envOnlyMode = false;
for (int j = 0; j < 3; j++) {
    if (vol[j] & 0x10) {
        uint8_t v = vol[j] & 0x0F;
        if (v > maxVol) maxVol = v;
        if (!toneOn[j] && !noiseOn[j]) envOnlyMode = true;
    }
}
if (envOnlyMode)
    snprintf(volStr, sizeof(volStr), "ENV");
else
    snprintf(volStr, sizeof(volStr), "%d", maxVol);
```

当 envOnlyMode=true 时，ch0-2 对应通道的音量条按满幅度显示。

## 四、钢琴键盘

- env mode + tone/noise 全关 → 根据 channelLevel 亮起，满幅度
- DAC 模式 → 同理正常亮起

## 验证

1. 方波模式：tone on + vol > 0 → 音量条显示，文字显示 volume 数字
2. 噪声模式：noise on + vol > 0 → ch0-2 音量条显示，ch3 显示对应最大 volume
3. Env 模式：vol bit4=1 + tone/noise on → ch0-2 音量条显示，ch4 显示最大 volume
4. DAC 模式：tone+noise off + vol > 0 → ch0-2 音量条显示真实音量，文字 "DAC:vol"
5. ENV-only：tone+noise off + bit4=1 → ch0-2 满幅度，文字 "ENV"，ch4 显示 "ENV"
6. 钢琴键盘在 env-only 和 DAC 模式下正常亮起
