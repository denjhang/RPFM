# Core 1 VGM Player — MegaGRRL-style cycle counter timing

## 状态：已实现 ✅

## Context

VGM 播放最初用 `hardware_alarm` ISR 在 Core 0 运行，存在以下问题：
- ISR 中 AY8910 PIO 写入消耗 CPU 时间，影响 alarm 调度精度
- 即使非阻塞推 FIFO，FIFO drain 等待仍占时间
- USB HID 中断抢占（即使 timer IRQ 优先级提到 0x00）
- 调整 PIO 延时直接影响 PWM 音高，说明写入本身阻塞时序

参考 MegaGRRL（ESP32）方案：`D:\working\vscode-projects\Reference_Project\megagrrl-driver-revamp-2-20240226-saa\firmware\main\driver.c`
- MegaGRRL 用 CPU1 独占轮询 + cycle counter，零中断，时序完美
- RP2350A 同样双核，Core 0 跑 USB，Core 1 专门跑 VGM

## 最终实现

### Core 分工

- **Core 0**: USB HID callback → 写 cmd_buf，tud_task()，LED，WS2812，deferred register writes（实时模式）
- **Core 1**: VGM 播放 tight loop，用 `timer_hw->timerawl` 读 cycle counter，解析 VGM 命令，直接写 PIO

### Core 1 主循环（参考 MegaGRRL Driver_Main）

```
void core1_vgm_main() {
    uint64_t cycle = 0;
    uint64_t last_cc = timer_hw->timerawl;  // 1µs counter
    uint64_t next_sample = 0;               // sample# for next VGM command

    while (true) {
        if (!(s_status & STATUS_PLAYING)) {
            last_cc = timer_hw->timerawl;
            cycle = 0;
            continue;
        }

        uint64_t cc = timer_hw->timerawl;
        cycle += (cc - last_cc);
        last_cc = cc;
        uint64_t sample = cycle * 44100 / 1000000;  // µs → samples

        if (sample >= next_sample) {
            while (true) {
                uint8_t cmd;
                if (!cmd_buf_read(&s_cmd, &cmd)) {
                    s_status &= ~STATUS_PLAYING;  // underrun
                    break;
                }
                if (cmd == 0xA0) {
                    uint8_t reg, data;
                    cmd_buf_read(&s_cmd, &reg);
                    cmd_buf_read(&s_cmd, &data);
                    write_reg_ay(0, reg, data);
                }
                else if (cmd == 0x61) { ... next_sample = sample + wait; break; }
                // ... 其他命令同现有 vgm_player.h
            }
        }
    }
}
```

关键点：
- `timer_hw->timerawl` 是 64-bit µs 计数器，双核都能读，不受中断影响
- `write_reg_ay` 在 Core 1 执行，阻塞不影响 Core 0 的 USB
- 恢复 `pio_bus_put` blocking 版本 + busy_wait，Core 1 不怕阻塞
- AY8910 PIO SM 回到全速（clkdiv=1），时序由软件控制

### Core 间通信

- **cmd_buf**: lock-free ring buffer（volatile head/tail），Core 0 写 head，Core 1 读 tail
- **s_status**: volatile uint8_t，Core 0 设 STATUS_PLAYING，Core 1 清除
- **s_ay_delay_100ns**: volatile，Core 0 HID 命令修改，Core 1 读取
- **s_vgm_loop_offset**: volatile，CMD_VGM_START 时设置

不需要 FIFO 或 mutex——volatile 变量 + ring buffer 已经 lock-free。

## 具体改动

### 1. `src/rpfm/vgm_player.h` — 重写

删除 `hardware_alarm` 相关代码（timer callback、irq_set_priority、hardware_alarm_claim），替换为 Core 1 tight loop。

- 保留 VGM_CMD_LEN 表
- 包含 `core1_vgm_main()` 函数
- `vgm_player_init()` 保留（存 buf/pio/sm 指针）
- `vgm_player_start()` 只设 `s_status |= STATUS_PLAYING` + 清 cycle
- `vgm_player_stop()` 只清 `s_status &= ~STATUS_PLAYING`

### 2. `src/rpfm/main.c` — 改动

- 保留 deferred queue（用于 YM2413 和实时模式 CMD_WRITE_AY）
- `main()` 中调用 `multicore_launch_core1(core1_vgm_main)` 启动 Core 1
- CMD_VGM_START: 设 loop_offset + `s_status |= STATUS_PLAYING`
- CMD_VGM_STOP: `s_status &= ~STATUS_PLAYING` + 清 cmd_buf
- 删除 `#include "hardware/irq.h"`（不再需要 IRQ 优先级设置）

### 3. `src/rpfm/spfm_bus.pio` — 改动

- `ay8910_pio_write_reg` 恢复 blocking 版本（`pio_bus_put` + busy_wait/NOP）
- Core 1 不怕阻塞，时序精确
- 删除 `ay8910_set_clkdiv`
- `ay8910_sm_init` 改回全速

### 4. 不改动

- `protocol.h`、`command_buffer.h`、`pio_cs.pio`、`ws2812.pio`、上位机全部不变

## 文件清单

| 文件 | 改动 |
|------|------|
| `src/rpfm/vgm_player.h` | 重写：hardware_alarm → Core 1 cycle counter loop |
| `src/rpfm/main.c` | 加 multicore_launch_core1，简化 CMD_VGM_START/STOP |
| `src/rpfm/spfm_bus.pio` | AY8910 写回 blocking + busy_wait，删除 clkdiv |

## 验证

1. 烧录后基本功能不变：USB HID 连接、实时模式写寄存器
2. 缓冲模式：Load VGM → Play → 声音稳定无降调
3. 调整 PIO delay 滑块，声音时序跟随变化
4. Stop → 声音立即停止
5. 大量密集鼓声数据不再拖慢
6. **单击即播放** — 之前需要多次点击的 bug 也同时解决
