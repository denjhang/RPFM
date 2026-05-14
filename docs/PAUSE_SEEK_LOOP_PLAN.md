# 暂停、跳转（Seek）、循环功能

## Context

缓冲模式缺少三个基本播放功能：
1. **暂停**：当前只停了上位机线程，固件 Core 1 继续消费缓冲区，导致 resume 后缓冲区已被吃空
2. **跳转**：UI 有进度条滑块但 `SeekUnifiedPlayback()` 是空桩
3. **循环**：当前循环由固件内部处理（32KB 环形缓冲区内 tail 跳转），上位机无法精确同步循环计数和时间轴

### 核心设计决策

- **循环**：上位机加载 VGM 时在内存中展开完整长度（intro + loop × maxLoops），作为普通线性 VGM 发给固件，固件 loopOff=0 不循环
- **跳转**：数据已展开为线性，seek 就是普通的停止→清空→重灌
- **固件只改暂停**，其他不动

## 改动文件

| 文件 | 改动 |
|------|------|
| `src/rpfm/protocol.h` | 新增 `CMD_VGM_PAUSE 0x09` |
| `src/rpfm/vgm_player.h` | Core 1 暂停标志 + pause/resume API |
| `src/rpfm/main.c` | HID 回调处理 `CMD_VGM_PAUSE` |
| `RPFM_Player/src/rpfm_protocol.h` | 新增 `CMD_VGM_PAUSE` |
| `RPFM_Player/src/rpfm_hid.h` | 新增 `rpfm_vgm_pause()` / `rpfm_vgm_resume()` |
| `RPFM_Player/src/rpfm_hid.cpp` | 实现 pause/resume HID 发送 |
| `RPFM_Player/src/ay8910_window.cpp` | 循环展开 + PauseVGMPlayback + SeekVGM + 线程 seek 支持 |
| `RPFM_Player/src/core/vgm_player_ui.h` | 回调 struct 加 `seekToPosition` + `vgmLoopEnabled` |
| `RPFM_Player/src/core/vgm_player_ui.cpp` | 进度条滑块接回调 + 循环 checkbox |

## 一、固件暂停 (`CMD_VGM_PAUSE 0x09`) — 唯一的固件改动

暂停**不清空缓冲区**，只让 Core 1 停止消费。

### `vgm_player.h`

在 STATUS_PLAYING 检查之后、时间累积之前加暂停检查：

```c
static volatile bool s_vgm_paused = false;

// 在 core1_vgm_main，s_vgm_started 检查之后：
if (s_vgm_paused) {
    last_cc = timer_hw->timerawl;  // 冻结时间
    continue;
}

static inline void vgm_player_pause(void)  { s_vgm_paused = true; }
static inline void vgm_player_resume(void) { s_vgm_paused = false; }
```

### `main.c` HID 回调

```c
case CMD_VGM_PAUSE:
    if (len >= 1) {
        if (payload[0]) vgm_player_pause();
        else vgm_player_resume();
    }
    break;
```

### 上位机 HID 层

`rpfm_hid.cpp` 新增 `rpfm_vgm_pause()` / `rpfm_vgm_resume()`。

## 二、循环展开（上位机内存展开）

### 概念

VGM 文件结构：
```
[0 ─── intro ──── loopPoint ──── loop]
                  ↑                           ↑
            totalSamples              loopSamples
```

展开后（maxLoops=2 为例）：
```
[intro][loop][loop]
0     ↑    ↑
  intro  intro+loop
         intro+loop×2 = totalExpanded
```

**公式**：`totalExpanded = (totalSamples - loopSamples) + loopSamples × maxLoops`

当 `maxLoops = 0`（无限循环）时，展开次数取一个合理值（如 99）。

### 实现

在 `LoadVGMFile` 中，读取 VGM 头部的 `numTicks`（offset 0x18）、`loopOfs`（offset 0x1C）、`loopTicks`（offset 0x20）。如果有循环（loopTicks > 0 且 loopOfs > 0），在 `s_memData` 中复制循环段 N-1 次追加到末尾：

```c
if (s_vgmLoopSamples > 0 && s_vgmLoopOffset > 0 && s_vgmMaxLoops > 0) {
    // 循环段数据：从 loopOffset 到文件末尾
    size_t loopDataLen = s_memData.size() - s_vgmLoopOffset;
    // 追加 N-1 次（第一次已经在原始数据中）
    size_t origSize = s_memData.size();
    size_t newSize = origSize + loopDataLen * (s_vgmMaxLoops - 1);
    s_memData.resize(newSize);
    for (int i = 1; i < s_vgmMaxLoops; i++) {
        memcpy(&s_memData[origSize + loopDataLen * (i-1)],
               &s_memData[s_vgmLoopOffset], loopDataLen);
    }
    // 更新 totalSamples 为展开后长度
    s_vgmTotalSamples = s_vgmTotalSamples
                      + s_vgmLoopSamples * (s_vgmMaxLoops - 1);
}
```

### 固件 loopOff

展开后，发 CMD_VGM_START 时 `loopOff = 0`（不循环），因为上位机已经展开了全部数据。

## 三、PauseVGMPlayback 改造

```c
static void PauseVGMPlayback(void) {
    if (!s_vgmLoaded || !s_vgmPlaying) return;
    s_vgmPaused = !s_vgmPaused;

    if (s_connected && s_playbackMode == 1) {
        if (s_vgmPaused) {
            rpfm_vgm_pause();       // 固件停止消费，缓冲区保留
            key_off_all();           // mixer = 0x3F 静音
        } else {
            ApplyShadowState();      // 恢复影子寄存器到硬件
            rpfm_vgm_resume();       // 固件继续消费
        }
    }
}
```

## 四、Seek/跳转

数据已展开为线性，seek 是普通的停止→清空→重灌，无循环特殊处理。

### SeekVGM

```c
static volatile size_t s_vgmSeekPos = 0;

static void SeekVGM(uint32_t targetSample) {
    targetSample = min(targetSample, s_vgmTotalSamples);

    // 1. 停止上位机线程
    s_vgmStreamRunning = false; s_vizRunning = false;
    // WaitForSingleObject + CloseHandle ...

    // 2. 停固件 + 清空缓冲区
    if (s_connected) { rpfm_vgm_stop(); InitHardware(); }

    // 3. 静默快进到目标 sample（数据已展开，0x66 不会触发循环）
    VizVGMReader seekReader;
    seekReader.open(s_vgmPath, s_vgmDataOffset);
    uint32_t skipSamples = 0;
    while (skipSamples < targetSample) {
        int w = VizProcessCommand(seekReader);
        if (w < 0) break;
        if (w > 0) { skipSamples += w; if (skipSamples > targetSample) { skipSamples = targetSample; break; } }
    }

    // 4. 更新位置
    s_vgmCurrentSamples = targetSample;
    s_fwTick = targetSample;
    s_vgmLoopCount = 0; s_vizLoopCount = 0;
    s_fadeoutActive = false; s_fadeoutLevel = 1.0f;

    // 5. 写影子寄存器到硬件（固件已停止）
    if (s_connected) ApplyShadowState();

    // 6. 重启线程（从 seek 位置 prefill + 流式发送）
    s_vgmSeekPos = seekReader.pos;
    s_vgmPaused = false;
    // CreateThread ...
}
```

### 线程适配

`VGMStreamThread` 和 `VGMVisualizationThread` 读取 `s_vgmSeekPos` 作为起始位置。

### loopOff 重算

数据已展开，`loopOff = 0`，无需重算。

## 五、循环开关

### 状态

```c
static bool s_vgmLoopEnabled = true;
```

- `s_vgmLoopEnabled = true`：加载 VGM 时展开循环
- `s_vgmLoopEnabled = false`：不展开循环，播放到第一个 0x66 停止

### 影响

1. `LoadVGMFile` 中：`s_vgmLoopEnabled` 为 false 时跳过展开
2. UI：通过 `VGMPlayerCallbacks.vgmLoopEnabled` 指针操作
3. 固件：始终 loopOff=0（展开模式下不循环）

### UI

`vgm_player_ui.cpp` 在 "L:" 前加循环 checkbox。maxLoops 输入框保留（控制展开次数）。

## 六、实现顺序

1. **固件暂停** — protocol.h → vgm_player.h → main.c → rpfm_protocol.h → rpfm_hid.h/cpp
2. **循环展开** — ay8910_window.cpp (LoadVGMFile)
3. **PauseVGMPlayback 改造** — ay8910_window.cpp
4. **Seek** — ay8910_window.cpp (SeekVGM + 线程适配)
5. **循环开关** — ay8910_window.cpp → vgm_player_ui.h/cpp
6. **UI 接入** — vgm_player_ui.cpp (seek 回调 + loop checkbox)

## 七、验证

1. 暂停/恢复：缓冲模式暂停后声音停止，恢复后从暂停位置继续
2. 循环：开启时完整播放 intro+loop×N 后停止，进度条反映展开后总长
3. 循环开关：关闭后播完一次停止
4. 进度条拖动：松手后从目标位置继续播放，硬件状态正确
5. 边界：seek 到末尾、seek 到 0、暂停中 seek
6. Live 模式不受影响
7. 固件构建验证
