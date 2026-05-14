# 缓冲模式通道屏蔽

## 概述

缓冲模式提供两种通道屏蔽实现方式，通过侧边栏 Mute Mode 单选框切换：

| 模式 | 实现位置 | 延迟 | 说明 |
|------|---------|------|------|
| **Host** | 上位机 patchMute | 有延迟 | 修改 VGM 字节流后发送，unmute 需等固件缓冲区消化 |
| **Firmware** | 固件 mute_intercept | 瞬间 | 固件 Core 1 在寄存器写入前拦截，推荐模式 |

## Host 模式（数据流 patch）

### 实现位置

`ay8910_window.cpp` → `VGMStreamThread` 函数内。

### 核心机制

#### 1. 数据备份

线程启动时，`localData` 是 VGM 数据的本地副本。创建 `localDataOrig = localData` 保留原始未修改数据，用于 unmute 时恢复。

#### 2. patchMute lambda

扫描 VGM 字节流，识别 `0xA0` 命令（AY8910 寄存器写入），根据当前 `s_chMuted[]` 状态修改数据字节：

```
0xA0 [chipID|reg] [data]
       ^^^^^^^^^   ^^^^
       byte[1]     byte[2]
```

- `byte[1]` 高 1 bit = chipID (0 或 1)
- `byte[1]` 低 7 bit = 寄存器号

#### 方波 ch0-ch2 音量 (reg 0x08-0x0A)

```c
if (s_chMuted[base + ch]) {
    if (s_soloCh == base + 4 && (buf[p + 2] & 0x10)) { /* pass */ }
    else buf[p + 2] = 0;
}
```

#### Envelope ch4 (reg 0x08-0x0A)

```c
if (s_chMuted[base + 4]) buf[p + 2] &= ~0x10;
```

#### Mixer (reg 0x07)

```c
for (int ch = 0; ch < 3; ch++) {
    if (s_chMuted[base + ch]) {
        if (s_soloCh == base + 3 && !(buf[p + 2] & (1 << (ch + 3)))) { /* pass */ }
        else buf[p + 2] |= (0x09 << ch);
    }
}
if (s_chMuted[base + 3]) buf[p + 2] |= 0x38;
```

#### 3. VGM 命令跳转

| 命令 | 长度 | 处理 |
|------|------|------|
| 0x61 | 3 字节 | `p += 3` |
| 0x62 / 0x63 | 1 字节 | `p += 1` |
| 0x70-0x7F | 1 字节 | `p += 1` |
| 0x66 | EOF | `p = len` |
| 0x67 | 7 + size | 数据块 |
| 其他 | 1 字节 | `p += 1` |

#### 4. 时序

```
线程启动 (Host 模式)
  ├─ 备份: localDataOrig = localData
  ├─ 初始 patch: patchMute(&localData[localPos], localTotal)
  ├─ Phase 1: prefill (已 patch 的数据)
  ├─ rpfm_vgm_start()
  └─ Phase 2: 循环发送
       ├─ 每 60 字节: patchMute → rpfm_send_vgm_data()
       └─ muteDirty 时:
            ├─ memcpy 从 localDataOrig 恢复
            └─ patchMute 重新应用当前 mute 状态
```

#### 5. muteDirty 恢复流程

GUI 线程点击 mute/unmute → `ApplyChannelMute()` → 设置 `s_muteDirty = true`

流线程检测到 `muteDirty`：
1. `memcpy` 从 `localDataOrig` 恢复 `localPos` 到末尾的数据
2. `patchMute` 重新应用当前 `s_chMuted[]` 状态

恢复后的数据从 `localPos` 开始发送，固件缓冲区中残留的旧数据会自然消化。

### Host 模式限制

- **延迟**：mute/unmute 后需要等待固件缓冲区消化完旧数据才能完全生效
- **Solo E/N 条件放行**：solo E 时使用 envelope 的通道放行，solo N 时 noise 启用的通道 tone 放行

## Firmware 模式（固件拦截）

### 实现位置

- 固件：`vgm_player.h` → `mute_intercept()`
- 上位机：`ApplyChannelMute()` 通过 `CMD_SET_MUTE` 发送 mask

### 协议

`CMD_SET_MUTE (0x0A)`：payload = `[mask(1B), solo(1B)]`

- `mask`：bit0-2=tone chA/B/C, bit3=noise, bit4=envelope
- `solo`：-1=none, 0-4=独奏通道号

### 固件拦截逻辑

在 Core 1 VGM 循环的 0xA0 命令处理中，`write_reg_ay()` 之前调用 `mute_intercept(type_byte, data)`：

```c
static inline uint8_t mute_intercept(uint8_t type_byte, uint8_t data) {
    uint8_t mute = s_mute_mask;
    if (!mute && s_solo_ch < 0) return data;

    uint8_t reg = type_byte & 0x7F;  // strip chipID bit7
    // ... 拦截音量/mixer 寄存器
}
```

### 上位机 mask 构建

只取 chip1 的 5 个通道（单芯片），避免 chip2 通道通过 `%5` 覆盖 chip1：

```c
uint8_t mask = 0;
for (int j = 0; j < AY_CH_PER_CHIP; j++)
    if (s_chMuted[j]) mask |= (1 << j);
int8_t solo = (s_soloCh >= 0 && s_soloCh < AY_CH_PER_CHIP) ? s_soloCh : -1;
rpfm_set_mute(mask, solo);
```

### Firmware 模式特点

- **瞬间生效**：固件直接拦截，不受缓冲区影响
- **不修改 VGM 数据**：Firmware 模式下跳过上位机 patchMute，避免双重拦截
- **切换曲目自动复位**：`ResetState()` 时发送 `rpfm_set_mute(0, -1)` 清零固件 mute 状态
- **Live/Buffered 通用**：固件拦截在 Core 1 VGM 循环中，不区分播放模式

## 通道映射

每个 AY8910 芯片 5 个通道（`AY_CH_PER_CHIP = 5`）：

| 索引 | 名称 | 芯片 |
|------|------|------|
| 0 | ChA | chip 0 |
| 1 | ChB | chip 0 |
| 2 | ChC | chip 0 |
| 3 | Noise | chip 0 |
| 4 | Env | chip 0 |
| 5-9 | 同上 | chip 1 |

`base = chipID * AY_CH_PER_CHIP`
