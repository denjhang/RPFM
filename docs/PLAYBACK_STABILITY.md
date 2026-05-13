# RPFM Player 播放稳定性优化

## 问题

CPU 高负载时 VGM 播放卡住，必须重新点击文件才能恢复。

## 根因分析

### 1. HID 写入无重试

`HidD_SetOutputReport` 是同步阻塞调用，CPU 高时被 OS 延迟调度导致超时失败。
原代码失败后直接返回 false，上层无重试，播放线程直接退出。

### 2. 缓冲模式用 Sleep(1) 轮询

`VGMStreamThread` 的 backpressure 轮询用 `Sleep(1)`，CPU 高负载时实际睡 5-15ms，
导致数据补充不及时，固件 buffer 耗尽断音。

### 3. 播放线程退出后 UI 状态不一致

线程 break 退出后没有标记 `s_vgmPlaying=false`，UI 仍显示"播放中"但实际已停止。

## 优化方案

### HID 写入重试 (`rpfm_hid.cpp`)

```cpp
for (int retry = 0; retry < 3; retry++) {
    if (HidD_SetOutputReport(s_handle, buf, 65))
        break;
    if (retry == 2)
        return false;
    Sleep(1);
}
```

3 次重试，间隔 1ms。覆盖 CPU 短暂繁忙的场景。

### 缓冲模式多媒体定时器 (`ay8910_window.cpp`)

`VGMStreamThread` 启动时创建 1ms 多媒体定时器：

```cpp
HANDLE mmEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
MMRESULT mmTimer = timeSetEvent(1, 1, (LPTIMECALLBACK)mmEvent, 0,
                                 TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
```

backpressure 轮询用 `WaitForSingleObject(mmEvent, INFINITE)` 替代 `Sleep(1)`。
多媒体定时器精度 ~1ms，不受 CPU 负载影响。

### 播放线程失败恢复

| 阶段 | 原行为 | 新行为 |
|------|--------|--------|
| Prefill | 失败直接退出 | 连续失败 10 次 + Sleep(5) 重试 |
| 数据传输 | 失败 break 退出 | 连续失败 20 次 + Sleep(3) 重试 |
| Backpressure | 失败静默 | 失败时 Sleep(3) + 保持上次 bufLevel |
| 等待完成 | 失败静默 | 失败时 Sleep(3) 重试 |
| 线程退出 | 只设 streamRunning=false | 额外设 playing=false + trackEnded=true |

### 缓冲区范围调整

旧范围：64B ~ 2KB（6 档）
新范围：512B ~ 8KB（6 档：512B, 1KB, 2KB, 3KB, 4KB, 8KB）
默认：512B

下位机 command_buffer 为 32KB，8KB 上位机缓冲安全。

## 改动文件

- `RPFM_Player/src/rpfm_hid.cpp` — HID 写入 3 次重试
- `RPFM_Player/src/ay8910_window.cpp` — 多媒体定时器 + 失败恢复 + 缓冲范围
