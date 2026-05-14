# 软件音频仿真器 + PWM 混音框架

## Context

RPFM 当前只支持 AY8910（PIO 硬件驱动）和 YM2612 DAC（GPIO22 PWM 直出）。VGM 格式支持数十种芯片，其中稀有芯片没有硬件对应，只能软件仿真后通过 PWM 输出。

**设计决策**：
- 仅仿真**纯合成芯片**（自带波形生成，无外部采样 ROM 依赖）
- 依赖大采样 ROM 的芯片（OPL4 2MB、YM2610 16MB、OKIM6295 等）通过 OPNB2OPN2 预处理为 8-bit DAC 流，固件不仿真
- 仿真器以 22050Hz 渲染（OPNB2OPN2 已验证此采样率对波形表芯片够用），与 VGM 44100Hz 主时钟解耦
- Live 模式暂不支持软件仿真器，仅缓冲模式

**目标芯片（用户指定顺序）**：SCC → FDS → NES → GameBoy → WSwan → HuC6280

## 架构概览

```
VGM 字节流 → Core 1 命令分发
                ├── 0xA0 → AY8910 PIO 硬件（已有，44100Hz）
                ├── 0x52 → YM2612 DAC → mixer.dac_sample
                ├── 0x5B → SCC 仿真器.write_reg()
                ├── 0x5C → FDS 仿真器.write_reg()
                ├── 0xB0 → NES 仿真器.write_reg()
                ├── 0xB3 → HuC6280 仿真器.write_reg()
                ├── 0xB4 → WSwan 仿真器.write_reg()
                └── ... 其他合成芯片
                ↓ (wait 边界，44100Hz)
           所有活跃仿真器.render() → mixer → dac_write(pwm_level)
```

## 一、仿真器接口 (`src/rpfm/spfm/emu_common.h`)

```c
typedef struct spfm_emu {
    uint8_t  chip_id;
    bool     active;
    uint8_t  volume;           // 0-255, 默认 128
    void    *state;

    // 事件驱动：VGM 命令到达时调用
    void  (*write_reg)(void *state, uint8_t reg, uint8_t data);
    // 每采样边界 (22050Hz)：返回 int8_t（-128..+127，0=静音）
    int8_t (*render)(void *state);
    // 初始化 / 复位
    void  (*init)(void *state, uint32_t clock_hz);
    void  (*reset)(void *state);
} spfm_emu_t;
```

关键设计：
- **int8_t render 输出**：匹配 8-bit PWM 分辨率，mixer 加 128 转 unsigned
- **纯整数运算**：RP2350 无 FPU
- **active=false 时不调用 render**：零开销
- **不需要采样 ROM 参数**：纯合成芯片自带波形

## 二、混音器 (`src/rpfm/spfm/mixer.h`)

```c
#define MIXER_MAX_SOURCES 8

typedef struct {
    spfm_emu_t *sources[MIXER_MAX_SOURCES];
    int         active_count;
    volatile int8_t dac_sample;   // 0x52 DAC 直通
    volatile bool   dac_active;
} mixer_t;

uint8_t mixer_render(mixer_t *m);
```

混音算法：
```c
int16_t mix = 0;
for (int i = 0; i < active_count; i++) {
    mix += (int16_t)sources[i]->render() * sources[i]->volume / 128;
}
if (dac_active) mix += dac_sample;
mix = clamp(mix, -128, 127);
return (uint8_t)(mix + 128);
```

## 三、Core 1 循环改造 (`vgm_player.h`)

当前 0x52 DAC 直接 `dac_write(data)`，需改为写入 `mixer.dac_sample`。

循环末尾（wait 边界）新增：
```c
uint8_t output = mixer_render(&s_mixer);
dac_write(output);
```

## 四、VGM 命令分发表

```c
typedef struct {
    uint8_t cmd_lo, cmd_hi;
    spfm_emu_t *emu;
} vgm_dispatch_t;
```

| VGM 命令 | 芯片 | 实现顺序 |
|----------|------|----------|
| 0x5B | SCC (Konami) | P1 |
| 0x5C | FDS (NES) | P2 |
| 0xB0 | NES 2A03 | P3 |
| 0xB4 | WSwan | P4 |
| 0xB3 | HuC6280 (PCE) | P5 |
| 0x50 | SN76489 | P6 |

## 五、协议新增 (`protocol.h`)

```c
#define CMD_SET_ACTIVE_CHIPS 0x0B // [mask(4B LE)] — 激活哪些仿真器
```

仅一个新命令，无需数据块传输（纯合成芯片无采样 ROM）。

## 六、内存预算

| 组件 | 大小 |
|------|------|
| 现有（代码+缓冲区+栈+SDK） | ~172KB |
| 仿真器状态（6个芯片） | ~2KB |
| 混音器+分发表 | ~0.2KB |
| **合计** | ~174KB / 520KB |
| **剩余** | ~346KB |

无采样数据池，内存极其宽裕。

## 七、文件结构

```
src/rpfm/
  main.c, protocol.h, command_buffer.h  (修改)
  vgm_player.h                          (修改：混音集成)
  spfm/
    emu_common.h    — 接口定义
    mixer.h         — 混音总线
    scc.c/.h        — Konami SCC 仿真
    fds.c/.h        — NES FDS 仿真
    nes.c/.h        — NES 2A03 仿真
    gb.c/.h         — GameBoy DMG 仿真
    wswan.c/.h      — WonderSwan 仿真
    huc6280.c/.h    — HuC6280 (PCE) 仿真
```

## 八、实现顺序

1. **框架基础**（无仿真器）：emu_common.h + mixer.h + vgm_player.h 混音集成 + 0x52 改为 mixer 直通 + CMD_SET_ACTIVE_CHIPS + 验证现有功能不变
2. **SCC**（用户指定第一个）：5ch 波形表，VGM 命令 0x5B，最简单的波形表芯片
3. **FDS**：单声道波形表 + 调制
4. **NES 2A03**：2 方波 + 1 三角波 + 1 噪声 + 1 DMC
5. **GameBoy DMG**：2 方波 + 1 噪声
6. **WSwan**：4ch 波形 + 噪声
7. **HuC6280**：6ch 波形表，最复杂的波形表芯片

## 九、上位机侧边栏 UI (`ay8910_window.cpp`)

### 新增状态

```c
static bool s_emuEnabled = false;   // 仿真模式总开关
// 以下在 LoadVGMFile 时根据 VGM 头部芯片时钟字段填充
static bool s_vgmHasSCC = false;
static bool s_vgmHasFDS = false;
static bool s_vgmHasNES = false;
static bool s_vgmHasGB = false;
static bool s_vgmHasWS = false;
static bool s_vgmHasPCE = false;
static int s_emuChipCount = 0;      // 需要仿真的芯片数量
```

### VGM 头部芯片检测

`LoadVGMFile` 中读取 VGM 头部各芯片时钟字段（非零 = 芯片存在），填充上述标志：
- Offset 0x84: HuC6280
- Offset 0x88: K053260 (SCC 检测用 0x5B 命令扫描更可靠)
- 其他芯片通过扫描 VGM 命令流中的芯片专用 opcode 检测

### 侧边栏 UI（在 Mute Mode 下方）

```cpp
ImGui::Separator();
ImGui::TextDisabled("Emulation");
ImGui::Checkbox("Enable##emu", &s_emuEnabled);
if (s_emuEnabled) {
    ImGui::Indent();
    if (s_vgmLoaded && s_emuChipCount > 0) {
        ImGui::Text("%d chip(s) detected:", s_emuChipCount);
        if (s_vgmHasSCC)   ImGui::TextDisabled("  SCC");
        if (s_vgmHasFDS)   ImGui::TextDisabled("  FDS");
        if (s_vgmHasNES)   ImGui::TextDisabled("  NES 2A03");
        if (s_vgmHasGB)    ImGui::TextDisabled("  GameBoy");
        if (s_vgmHasWS)    ImGui::TextDisabled("  WSwan");
        if (s_vgmHasPCE)   ImGui::TextDisabled("  HuC6280");
    } else {
        ImGui::TextDisabled("No emulatable chips");
    }
    ImGui::Unindent();
}
```

### Config 持久化

- `LoadConfig`: 读取 `EmuEnabled`
- `SaveConfig`: 写入 `EmuEnabled`

## 十、验证

1. Phase 1 完成后：现有 AY8910 + YM2612 DAC 播放行为完全一致
2. SCC：Konami VGM 正确发声
3. 混音验证：AY8910 硬件 + SCC 仿真器同时输出，PWM 混音无爆音
4. 性能：Core 1 负载 < 50%
