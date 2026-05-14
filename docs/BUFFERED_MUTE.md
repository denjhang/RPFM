# 缓冲模式通道屏蔽

## 概述

缓冲模式下 VGM 原始字节流直接灌给固件，上位机无法拦截固件的寄存器写入。采用**数据流 patch** 方式：在发送给固件之前，扫描并修改 VGM 字节流中 AY8910 寄存器写入命令的数据字节。

## 实现位置

`ay8910_window.cpp` → `VGMStreamThread` 函数内。

## 核心机制

### 1. 数据备份

线程启动时，`localData` 是 VGM 数据的本地副本。创建 `localDataOrig = localData` 保留原始未修改数据，用于 unmute 时恢复。

### 2. patchMute lambda

扫描 VGM 字节流，识别 `0xA0` 命令（AY8910 寄存器写入），根据当前 `s_chMuted[]` 状态修改数据字节：

```
0xA0 [chipID|reg] [data]
       ^^^^^^^^^   ^^^^
       byte[1]     byte[2]
```

- `byte[1]` 高 7 bit = chipID (0 或 1)
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

### 3. VGM 命令跳转

`patchMute` 必须正确跳过非 `0xA0` 命令，否则会误解析：

| 命令 | 长度 | 处理 |
|------|------|------|
| 0x61 | 3 字节 | `p += 3` |
| 0x62 / 0x63 | 1 字节 | `p += 1` |
| 0x70-0x7F | 1 字节 | `p += 1` |
| 0x66 | EOF | `p = len` |
| 0x67 | 7 + size | 数据块，`p += 7 + size` |
| 其他 | 1 字节 | `p += 1` |

### 4. 时序

```
线程启动
  ├─ 备份: localDataOrig = localData
  ├─ 初始 patch: patchMute(&localData[localPos], localTotal)
  ├─ Phase 1: prefill (已 patch 的数据)
  ├─ rpfm_vgm_start()
  └─ Phase 2: 循环发送
       ├─ 每 60 字节: patchMute(&localData[localPos], toSend) → rpfm_send_vgm_data()
       └─ muteDirty 时:
            ├─ memcpy 从 localDataOrig 恢复
            └─ patchMute 重新应用当前 mute 状态
```

### 5. muteDirty 恢复流程

GUI 线程点击 mute/unmute → `ApplyChannelMute()` → 设置 `s_muteDirty = true`

流线程检测到 `muteDirty`：
1. `memcpy` 从 `localDataOrig` 恢复 `localPos` 到末尾的数据
2. `patchMute` 重新应用当前 `s_chMuted[]` 状态

恢复后的数据从 `localPos` 开始发送，固件缓冲区中残留的旧数据会自然消化。

### 6. 通道映射

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

## 限制

- **延迟**：mute/unmute 后需要等待固件缓冲区消化完旧数据才能完全生效，延迟与缓冲区大小相关
- **Solo E/N 条件放行**：与 Live 模式一致，solo E 时使用 envelope 的通道放行，solo N 时 noise 启用的通道 tone 放行
- **不影响 Live 模式**：`patchMute` 仅在 `VGMStreamThread`（buffered 模式）中使用
