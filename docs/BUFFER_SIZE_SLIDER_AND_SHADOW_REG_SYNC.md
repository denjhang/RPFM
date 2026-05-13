# 缓冲区大小滑块 + Tick 同步影子寄存器

## 状态：已实现 ✅

## Context

缓冲模式有两个问题：
1. 缓冲区固定 32KB，实际使用量通常只有几百字节到几 KB，无法调整
2. 缓冲模式没有影子寄存器更新，UI 上看不到钢琴键盘/电平表/音高信息
3. 上位机可视化和硬件播放之间存在延迟

## 一、缓冲区大小滑块

### 方案

**不改固件**：缓冲区大小由固件编译时决定（CMD_BUF_SIZE）。上位机滑块只控制上位机端的发送策略（预填量、背压阈值）。

`RPFM_Player/src/ay8910_window.cpp`:
- 侧边栏加滑块 "Buffer Size"
- 范围：64B / 128B / 256B / 512B / 1KB / 2KB
- 影响 VGMStreamThread 的 `prefillAmount` 和背压阈值
- 持久化到 config

## 二、Tick 同步影子寄存器

### 方案

固件 Core 1 VGM 播放器回报当前播放 tick（sample position），上位机根据实际 tick 决定何时 apply 影子寄存器更新，实现可视化与硬件精确同步。

### 固件改动

`src/rpfm/vgm_player.h`:
- 新增 `volatile uint32_t s_vgm_tick` — Core 1 每处理完一批 VGM 命令后写入 `next_sample`
- 播放开始时 `s_vgm_tick = 0`

`src/rpfm/main.c`:
- HID 响应帧 bytes[4-7] 写入 `s_vgm_tick`（32-bit LE）
- 原有 bytes[0-3] 不变（ack_seq, status, buf_level）

### 上位机解析状态机

VGMStreamThread 在发送 VGM 字节流的同时，同步解析命令追踪 sample tick：

```cpp
struct VgmParseState {
    bool in_cmd = false;
    uint8_t cmd = 0;
    uint8_t bytes_remaining = 0;
    uint8_t cmd_buf[4];
    int buf_pos = 0;
    uint32_t currentTick = 0;  // 追踪 sample 位置
};
```

- 0x61 命令：`currentTick += lo | (hi << 8)`
- 0x62 命令：`currentTick += 735`（NTSC frame）
- 0x63 命令：`currentTick += 882`（PAL frame）
- 0x70-0x7F 命令：`currentTick += (cmd & 0x0F) + 1`
- 0xA0 命令：入队 `{reg, data, chipID, currentTick}`

### Tick 驱动延迟队列

```cpp
struct ShadowRegDelayQueue {
    // 每个 update 带 VGM sample tick
    void push(uint8_t reg, uint8_t data, uint8_t chipID, uint32_t tick);
    // 只 apply tick <= fwTick 的更新
    void flushTo(uint32_t fwTick);
};
```

VGMStreamThread 每次与固件 HID 通信时获取 `fwTick`，调 `flushTo(fwTick)` 只 apply 固件已经播放到的位置。

### 通信路径

```
上位机解析 VGM → {reg, data, chipID, tick} 入队
                        ↓
固件 HID 响应 → fwTick（当前播放 sample 位置）
                        ↓
flushTo(fwTick) → 只 apply tick <= fwTick 的更新到影子寄存器
                        ↓
UI 读取影子寄存器 → 钢琴键盘/电平表/音高显示
```

### 侧边栏 Tick 显示

缓冲模式播放时显示：
```
Tick: 12345 / 88200
Time: 0.3s / 2.0s
```

## 文件清单

| 文件 | 改动 |
|------|------|
| `src/rpfm/vgm_player.h` | 新增 `s_vgm_tick`，Core 1 每轮写入 |
| `src/rpfm/main.c` | HID 响应帧 bytes[4-7] 写入 tick |
| `RPFM_Player/src/rpfm_hid.h` | `rpfm_resp_t` 加 `tick` 字段，`rpfm_send_vgm_data` 加 tick 参数 |
| `RPFM_Player/src/rpfm_hid.cpp` | 解析 HID 响应 byte[4-7] 为 tick |
| `RPFM_Player/src/ay8910_window.cpp` | VGM 解析状态机追踪 tick + tick 驱动延迟队列 + 缓冲区滑块 + 侧边栏显示 |

## 验证

1. 缓冲区滑块：64B-2KB 范围，调整后播放正常
2. 影子寄存器：缓冲模式下钢琴键盘、音高显示、电平表与硬件声音同步
3. Tick 显示：侧边栏显示固件当前 tick 和总 tick，与实际播放位置一致
4. 切换回实时模式不受影响
