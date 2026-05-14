# Changelog

## [Unreleased]

### Added
- **缓冲模式暂停**：暂停 = `StopVGMPlayback` + 记住位置，恢复 = `SeekVGM(s_vgmCurrentSamples)` 从暂停位置快进后继续播放，无需固件暂停命令
- **缓冲模式跳转（Seek）**：`SeekVGM` 停止固件→静默快进 VGM 到目标 sample→`ApplyShadowState` 恢复芯片状态→`StartVGMPlayback` 从跳转位置重启线程，线程起始字节偏移通过 `CreateThread` LPVOID 参数传递（避免共享变量竞态）
- **循环展开**：`LoadVGMFile` 中将 VGM 循环段在内存中复制 N-1 次，展开为线性数据，固件 `loopOff=0` 不循环，进度条和 seek 统一在展开后的线性时间轴上操作
- **循环开关**：`s_vgmLoopEnabled` 控制是否展开循环，关闭时播放到第一个 `0x66` 停止，UI checkbox
- **进度条 seek 回调**：`VGMPlayerCallbacks.seekToPosition` + `vgmLoopEnabled` 指针接入 UI
- **Live 模式通道屏蔽**：方波 ch0-ch2 音量写 0 + mixer 禁用 tone/noise；Noise ch3 mixer 禁用全部 noise 位 (0x38)；Envelope ch4 音量寄存器清除 bit4。Solo E/N 条件放行：solo E 时使用 envelope 模式的方波通道不被拦截，solo N 时 mixer 中 noise 启用的通道 tone 位不被拦截。参考 MDPlayer `setAY8910Register` 实现
- **缓冲模式通道屏蔽**：`VGMStreamThread` 启动时备份原始数据 `localDataOrig`，发送前用 `patchMute` lambda 扫描修改 0xA0 命令中的音量/mixer 字节。`s_muteDirty` 标志触发时从备份恢复再重新 patch，支持 mute/unmute 切换。屏蔽逻辑与 Live 模式一致，参考 `CHANNEL_MUTE.md`
- **固件级通道屏蔽**：新增 `CMD_SET_MUTE (0x0A)` 命令，固件 Core 1 VGM 循环中 `mute_intercept()` 在 `write_reg_ay()` 前拦截音量/mixer 寄存器写入，瞬间生效无延迟。上位机侧边栏新增 Host/Firmware 单选框切换屏蔽模式，Live 和缓冲模式通用。Firmware 模式下跳过上位机 patchMute，避免双重拦截
- **YM2612 DAC 输出**：固件解析 VGM 0x52 命令时拦截 YM2612 DAC 寄存器 (0x2A/0x2B)，通过 GPIO22 硬件 PWM 输出 8-bit PCM 音频，支持 SNDH STE DMA 音频流
- **缓冲区扩容**：缓冲区滑块范围从 512B-8KB 扩展到 1KB-32KB（14 档），支持数据量大的曲目
- **VGM 流传输优化**：`VGMStreamThread` 每次迭代最多发送 8 帧（fire-and-forget 模式），每批仅轮询一次固件 buf_level/tick，减少 HID 同步读取开销，缓冲区未满时吞吐量显著提升

### Changed
- **文件夹历史**：上限从 50 条增加到 200 条
- **缓冲区默认值**：默认缓冲区从 8KB 调整为 6KB
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
