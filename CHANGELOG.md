# Changelog

## [Unreleased]

### Added
- **缓冲模式暂停**：暂停 = `StopVGMPlayback` + 记住位置，恢复 = `SeekVGM(s_vgmCurrentSamples)` 从暂停位置快进后继续播放，无需固件暂停命令
- **缓冲模式跳转（Seek）**：`SeekVGM` 停止固件→静默快进 VGM 到目标 sample→`ApplyShadowState` 恢复芯片状态→`StartVGMPlayback` 从跳转位置重启线程，线程起始字节偏移通过 `CreateThread` LPVOID 参数传递（避免共享变量竞态）
- **循环展开**：`LoadVGMFile` 中将 VGM 循环段在内存中复制 N-1 次，展开为线性数据，固件 `loopOff=0` 不循环，进度条和 seek 统一在展开后的线性时间轴上操作
- **循环开关**：`s_vgmLoopEnabled` 控制是否展开循环，关闭时播放到第一个 `0x66` 停止，UI checkbox
- **进度条 seek 回调**：`VGMPlayerCallbacks.seekToPosition` + `vgmLoopEnabled` 指针接入 UI

### Changed
- **文件夹历史**：上限从 50 条增加到 200 条
- **UI 暂停判断**：`isPaused` 不再依赖 `s_vgmPlaying`，暂停停止播放后 UI 仍能正确识别暂停状态

## [0.1] - 2026-05-13

### Added
- **Core 1 VGM 播放引擎**：MegaGRRL-style cycle counter，44100Hz sample-accurate 精确定时
- **Tick 同步**：固件 Core 1 回报 `s_vgm_tick`，上位机影子寄存器 tick-based 延迟队列同步
- **双核架构**：Core 0 USB HID + deferred writes，Core 1 VGM tight loop
- **USB HID 64B 帧协议**：替代 CDC，天然流控，CRC8 校验
- **缓冲区滑块**：侧边栏可调缓冲区大小，持久化到 `ay8910_config.ini`
- **Tick/Time 显示**：侧边栏显示固件 tick / 总 tick + 时间
- **HID 一键烧录**：BOOTSEL 命令自动进入烧录模式
- **ARY8910 PIO 写入时序**：可调 /WR 脉冲宽度（0-2000ns），独立 PIO SM + clock div 32
- **英文 README**：完整架构、协议、VGM 引擎、项目结构文档
- **中文 README**：`README_cn.md`

### Changed
- 项目重命名为 RPFM (Raspberry Pi Pico FM)
- SPFM 全称修正为 Serial Port FM
- PIO 并行总线驱动重构（14-bit + PIO1 CS）

## [0.0] - 2026-05-12

### Added
- PIO 14-bit 并行总线驱动 YM2413/AY8910
- 74LVC245 电平转换（3.3V → 5V）
- WS2812 片选活动指示 LED
- IC# 全局复位
- USB HID 设备枚举与通信
- Windows 上位机 (Dear ImGui) AY8910 控制窗口
- VGM 文件加载与播放（Live/Buffered 双模式）
- 钢琴键盘实时音高显示
- 电平表 + 寄存器表 + 示波器
