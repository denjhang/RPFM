# 缓冲模式进度跳转调试记录

## 问题

进度条拖动到任意位置后，音乐始终从头开始播放，seek 位置完全无效。

## 调试过程

### 第一步：怀疑固件/协议问题

最初怀疑固件 VGM 解析器在 seek 位置处解析错乱，或 USB HID 帧竞争。

**结论**：与固件无关。缓冲模式 seek 的本质和换曲一样——停止、清空缓冲区、重新灌数据。固件根本不知道发生了 seek。

### 第二步：加 DcLog 日志确认数据流

在 `SeekVGM`、`StartVGMPlayback`、`VGMStreamThread` 入口分别打印 `s_vgmSeekPos` 的值和地址。

关键日志：
```
[Seek] seekReader.pos=317500 memData.size=0     ← SeekVGM 设好了 s_vgmSeekPos
[Start] enter: s_vgmSeekPos=246793               ← StartVGMPlayback 入口正确
[Seek] after StartVGMPlayback: s_vgmSeekPos=246793
[VGM-Stream] thread start: seekPos=0             ← 线程读到 0！
```

同一地址 `00007ff77ff27a68`，主线程看到 246793，新线程看到 0。

### 第三步：定位根因——线程调度时序

`CreateThread` 返回后新线程**不会立即执行**。时序如下：

```
主线程                              新线程
─────────────────────                ──────────────────
SeekVGM: s_vgmSeekPos = 246793
StartVGMPlayback()
  CreateThread(VGMStreamThread) ──→  （尚未调度）
  ...其他代码...
StartVGMPlayback 返回
s_vgmSeekPos = 0  ← 清零！
                                    seekPos = s_vgmSeekPos  ← 读到 0
```

Windows 线程调度延迟导致新线程在主线程清零 `s_vgmSeekPos` **之后**才执行到第一行代码。

`volatile` 关键字只保证编译器不从寄存器缓存，不保证线程执行顺序。

### 第四步：修复

通过 `CreateThread` 的 `LPVOID param` 直接传值：

```cpp
// SeekVGM 中
s_startSeekPos = seekReader.pos;
StartVGMPlayback();

// StartVGMPlayback 中
size_t seekPos = s_startSeekPos;
s_startSeekPos = 0;
// ...
CreateThread(..., (LPVOID)(uintptr_t)seekPos, ...);  // 值在创建时就确定

// 线程入口
static DWORD WINAPI VGMStreamThread(LPVOID param) {
    size_t seekPos = (size_t)param;  // 不依赖全局变量
```

`LPVOID param` 是 `CreateThread` 调用时的值拷贝，不依赖任何全局变量的后续状态。

## 关键教训

1. **`CreateThread` 不等于线程立即执行**：Windows 线程调度不可预测，新线程可能在任意时刻被调度
2. **`volatile` 不解决时序问题**：它只防止编译器优化，不保证线程执行顺序
3. **跨线程传值用参数，不用共享状态**：如果线程启动时需要一个值，通过 `LPVOID param` 传入，不要依赖全局/共享变量
4. **缓冲模式 seek 和 Live 模式 seek 架构不同**：Live 模式线程持续运行，seek 只需移文件指针；缓冲模式需要停止→清空→重灌，涉及线程生命周期管理

## 最终 seek 流程

```
SeekVGM(targetSample)
  │
  ├─ 1. 停止所有线程 (WaitForSingleObject + CloseHandle)
  ├─ 2. rpfm_vgm_stop() + InitHardware()  ← 停固件 + 清缓冲区 + 复位芯片
  ├─ 3. VizVGMReader 快进到 targetSample  ← 更新 shadow 寄存器
  ├─ 4. ApplyShadowState()               ← 把快进后的 shadow 写到硬件
  └─ 5. StartVGMPlayback(seekPos)        ← 从跳转位置重启
         ├─ CreateThread(VGMStreamThread, param=seekPos)
         └─ CreateThread(VGMVisualizationThread, param=seekPos)
```

与正常播放的唯一区别：第 3、4 步快进并恢复硬件状态。线程启动方式和换曲完全相同。
