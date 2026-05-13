# 缓冲区大小滑块 + 影子寄存器可视化同步

## 状态：已实现 ✅

## Context

缓冲模式有两个问题：
1. 缓冲区固定 32KB，实际使用量通常只有几百字节到几 KB，无法调整
2. 缓冲模式没有影子寄存器更新，UI 上看不到钢琴键盘/电平表/音高信息
3. 上位机可视化和硬件播放之间存在延迟

## 一、缓冲区大小滑块

### 方案

**不改固件**：缓冲区大小由固件编译时决定（CMD_BUF_SIZE = 32KB）。上位机滑块控制上位机端的发送策略（预填量、背压阈值）。

`RPFM_Player/src/ay8910_window.cpp`:
- 侧边栏加滑块 "Buffer Size"
- 范围：512B / 1KB / 2KB / 3KB / 4KB / 8KB（默认 512B）
- 影响 VGMStreamThread 的 `prefillAmount` 和背压阈值
- 持久化到 config

## 二、缓冲模式可视化同步

### 方案：即时 apply（不用 tick 延迟队列）

缓冲模式下 `vgm_parse_byte` 解析到 AY8910 寄存器写入时直接调用 `UpdateAY8910State` / `UpdateAY8910State2`，不走 tick-based shadow queue。

**为什么不用 tick 同步**：

原方案用 `flushTo(fwTick)` 只 apply 固件已播放到的位置。但 fwTick 是固件 Core 1
实际播放位置，缓冲越大 fwTick 滞后越严重（8KB ≈ 180ms 延迟），导致可视化卡顿。

**为什么不改固件加回调**：

Core 1 是 44100Hz sample-accurate tight loop，任何中断/回调都会破坏时序精度。
固件 Core 1 不能被打断。

**当前方案**：

```
上位机解析 VGM → vgm_parse_byte → UpdateAY8910State (即时 apply)
                                    ↓
                         UI 读取影子寄存器 → 即时可视化
```

进度条仍用固件返回的 `s_vgm_tick`（真实播放位置），可视化跟数据发送同步。
可视化比声音超前一个缓冲区延迟（512B ≈ 10ms，用户无感）。

### 固件改动（tick 用于进度条）

`src/rpfm/vgm_player.h`:
- `volatile uint32_t s_vgm_tick` — Core 1 每处理完一批命令后写入 `next_sample`

`src/rpfm/main.c`:
- HID 响应帧 bytes[4-7] 写入 `s_vgm_tick`（32-bit LE）

### 侧边栏 Tick 显示

```
Tick: 12345 / 88200
Time: 0.3s / 2.0s
```

## 三、播放稳定性

详见 [PLAYBACK_STABILITY.md](PLAYBACK_STABILITY.md)。

核心改动：
- HID 写入 3 次重试（CPU 高负载时保底）
- 缓冲模式用 1ms 多媒体定时器替代 `Sleep(1)` 轮询
- 播放线程 HID 失败时重试而非直接退出

## 文件清单

| 文件 | 改动 |
|------|------|
| `src/rpfm/vgm_player.h` | `s_vgm_tick`，Core 1 每轮写入 |
| `src/rpfm/main.c` | HID 响应帧 bytes[4-7] 写入 tick |
| `RPFM_Player/src/rpfm_hid.h` | `rpfm_resp_t` 加 `tick` 字段 |
| `RPFM_Player/src/rpfm_hid.cpp` | 解析 HID 响应 byte[4-7] 为 tick + 写入重试 |
| `RPFM_Player/src/ay8910_window.cpp` | 缓冲区滑块 + 即时可视化 + 多媒体定时器 + 失败恢复 |

## 验证

1. 缓冲区滑块：512B-8KB 范围，调整后播放正常
2. 可视化：缓冲模式下钢琴键盘、电平表即时响应，不受缓冲大小影响
3. Tick 显示：侧边栏显示固件真实播放 tick
4. 稳定性：CPU 高负载时播放不中断
5. 切换回实时模式不受影响
