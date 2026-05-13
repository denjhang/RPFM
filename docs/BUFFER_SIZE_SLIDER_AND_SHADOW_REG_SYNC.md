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

### 方案：独立可视化线程

缓冲模式下启动独立 `VGMVisualizationThread`，自己打开 VGM 文件本地副本，按 44100Hz 真实速度独立解析 VGM，只调用 `UpdateAY8910State` / `UpdateAY8910State2` 更新影子寄存器，不发 HID 写入。

**演进历程**：

1. **v0：tick 延迟队列** — `flushTo(fwTick)` 只 apply 固件已播放到的位置。缓冲越大 fwTick 滞后越严重（8KB ≈ 180ms），可视化卡顿。
2. **v1：即时 apply** — 流线程推送 VGM 字节到环形队列，可视化线程消费队列解析。但队列数据来源于 HID 发送节奏（~1ms/帧），数据量小的 VGM 更新一卡一卡。
3. **v2（当前）：独立本地播放** — 可视化线程完全独立，自己读 VGM 文件按真实速度推进，和 live 模式同架构，不受 USB HID I/O 影响。

**为什么不改固件加回调**：

Core 1 是 44100Hz sample-accurate tight loop，任何中断/回调都会破坏时序精度。

**当前架构**：

```
VGMStreamThread (BELOW_NORMAL)          VGMVisualizationThread
    ↓ HID: VGM_DATA → 固件               ↓ 本地 VGM 文件副本
    ↓ s_fwTick (进度条)                   ↓ QPC + 1ms multimedia timer
    ↓                                      ↓ VizProcessCommand() → UpdateAY8910State()
    ↓                                      ↓ atomic_thread_fence(release)
    ↓                                      ↓
    ↓              GUI thread ←────────────┘
    ↓              UpdateChannelLevels()
    ↓              atomic_thread_fence(acquire)
    ↓              渲染
```

**跨线程内存可见性问题**：

Release 编译下编译器可能将影子寄存器的读取缓存到寄存器中，GUI 线程看不到可视化线程的写入，导致可视化不更新。解决方案：
- 可视化线程：写入影子寄存器后执行 `std::atomic_thread_fence(memory_order_release)`
- GUI 线程：读取影子寄存器前执行 `std::atomic_thread_fence(memory_order_acquire)`

**进度条**：仍用固件返回的 `s_fwTick`（真实播放位置），不受可视化线程影响。

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
| `RPFM_Player/src/ay8910_window.cpp` | 缓冲区滑块 + 独立可视化线程 + 内存屏障 + 多媒体定时器 + 失败恢复 |

## 验证

1. 缓冲区滑块：512B-8KB 范围，调整后播放正常
2. 可视化：缓冲模式下钢琴键盘、电平表响应速度与 live 模式一致，不受缓冲大小影响
3. Tick 显示：侧边栏显示固件真实播放 tick
4. 稳定性：CPU 高负载时播放不中断
5. 切换回实时模式不受影响
