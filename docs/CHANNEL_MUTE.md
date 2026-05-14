# Live 模式通道屏蔽

## 概述

AY8910 有 3 个方波通道（ch0-ch2），共享 1 个 noise 发生器和 1 个 envelope 发生器。上位机将 noise 和 envelope 分别映射为 ch3 和 ch4，共 5 个可屏蔽通道。

## 屏蔽逻辑（参考 MDPlayer setAY8910Register）

### 方波 ch0-ch2

拦截 `UpdateAY8910State` / `UpdateAY8910State2` 中的音量和 mixer 写入：

- **音量寄存器 R8-R10**：被 mute 的通道 `data = 0`（整个寄存器清零，包括 envelope 使能位 bit4）
- **Mixer 寄存器 R7**：被 mute 的通道 `data |= 0x09 << ch`（同时禁用 tone bit 和 noise bit）

### Noise ch3

- **Mixer 寄存器 R7**：`data |= 0x38`（禁用所有 3 个通道的 noise 位：bit3/4/5）
- Noise 是共享资源，无法只禁用单个通道的 noise

### Envelope ch4

- **音量寄存器 R8-R10**：`data &= ~0x10`（清除 bit4，禁用 envelope 模式，通道回退到固定音量）

## Solo 条件放行

Solo E 或 Solo N 时，需要条件判断允许相关通道通过，否则独奏没有声音输出。

### Solo E（ch4 或 ch4+5）

在音量寄存器拦截中：如果当前通道使用了 envelope 模式（原始 `data & 0x10` 为真），则不拦截该通道：

```c
if (s_chMuted[ch]) {
    if (s_soloCh == 4 && (data & 0x10)) { /* pass through */ }
    else data = 0;
}
```

### Solo N（ch3 或 ch3+5）

在 mixer 寄存器拦截中：如果该通道的 noise 位未禁用（`data` 中对应 noise bit 为 0），则不拦截该通道的 tone 位：

```c
if (s_chMuted[ch]) {
    if (s_soloCh == 3 && !(data & (1 << (ch + 3)))) { /* pass tone through */ }
    else data |= (0x09 << ch);
}
```

## ApplyChannelMute

点击 mute/solo 按钮时，`ApplyChannelMute(i)` 对已连接的硬件发送：

1. 对 ch0-ch2：写当前音量（mute 时写 0）或恢复原值
2. 写 mixer（追加被 mute 通道的禁用位）
3. 对 ch4（envelope mute）：清除所有音量寄存器的 bit4

## 缓冲模式

缓冲模式下 VGM 数据直接灌给固件，上位机无法拦截固件的寄存器写入。缓冲模式的通道屏蔽需要另行实现（数据流 patch 或固件支持）。
