# Changelog

## [Unreleased]

### Changed
- **缓冲模式可视化**：`vgm_parse_byte` 即时 apply 影子寄存器，不再使用 tick-based 延迟队列，消除大缓冲区的可视化延迟
- **缓冲区范围**：512B ~ 8KB（原 64B ~ 2KB），默认 512B
- **AY8910 电平表**：ch0-2 音量条反映真实音量寄存器值 (v/15)，vol=0 时快衰减；ch3 借用 noiseOn 通道最大音量；ch4 借用 ENV 通道最大音量（全部 vol=0 显示 "ENV" 满幅度）
- **钢琴键盘**：纯真实音量驱动不衰减，ENV 模式 vol=0 时满幅度亮起显示音高
- **电平表文字**：DAC 模式显示 "DAC:vol"，ENV 模式不再显示寄存器值，ch3/ch4 显示关联通道音量数字

### Added
- **播放稳定性**：HID 写入 3 次重试（1ms 间隔），覆盖 CPU 高负载场景
- **多媒体定时器**：缓冲模式 backpressure 轮询用 1mm 定时器替代 `Sleep(1)`
- **失败恢复**：Prefill 连续失败 10 次退出，数据传输连续失败 20 次停止，线程退出统一标记播放停止状态
- **GPIO 扩展规划**：RP2350A 实验版（触摸屏+SD卡）和 RP2350B 完整引脚分配文档

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
