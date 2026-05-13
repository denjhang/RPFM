# 缓冲区大小滑块 + 缓冲模式影子寄存器同步

## Context

缓冲模式有两个问题：
1. 缓冲区固定 32KB，实际使用量通常只有 14-16KB，无法调整
2. 缓冲模式没有影子寄存器更新，UI 上看不到钢琴键盘/电平表/音高信息

## 一、缓冲区大小滑块

### 方案

**不改固件**：缓冲区大小由固件编译时决定（32KB）。上位机滑块只控制上位机端的发送策略（预填量、背压阈值）。

`RPFM_Player/src/ay8910_window.cpp`:
- 新增 `static int s_bufTargetKB = 16;`，范围 2-32，步进 2
- 侧边栏加滑块 "Buffer Size"
- 影响 VGMStreamThread 的 `prefillAmount` 和背压阈值
- 持久化到 config

## 二、缓冲模式影子寄存器同步

### 方案

VGMStreamThread 在发送 VGM 字节流的同时，同步解析 0xA0 命令调用 UpdateAY8910State()。

### VGM 解析状态机

```cpp
struct VgmParseState {
    bool in_cmd = false;
    uint8_t cmd = 0;
    uint8_t bytes_remaining = 0;
    uint8_t cmd_buf[4];
    int buf_pos = 0;
};

void vgm_parse_byte(VgmParseState& s, uint8_t byte) {
    if (!s.in_cmd) {
        s.cmd = byte;
        s.buf_pos = 0;
        if (byte == 0xA0) {
            s.bytes_remaining = 2;  // reg_raw, data
            s.in_cmd = true;
        } else if (byte >= 0x70 && byte <= 0x7F) {
            // short wait, no data
        } else if (byte == 0x61) {
            s.bytes_remaining = 2;
            s.in_cmd = true;
        } else if (byte == 0x62 || byte == 0x63 || byte == 0x66) {
            // no data
        } else {
            uint8_t len = VGM_CMD_LEN[byte];
            if (len > 1) {
                s.bytes_remaining = len - 1;
                s.in_cmd = true;
            }
        }
    } else {
        s.cmd_buf[s.buf_pos++] = byte;
        s.bytes_remaining--;
        if (s.bytes_remaining == 0) {
            if (s.cmd == 0xA0) {
                uint8_t reg_raw = s.cmd_buf[0];
                uint8_t data = s.cmd_buf[1];
                uint8_t chipID = (reg_raw & 0x80) >> 7;
                uint8_t reg = reg_raw & 0x7F;
                if (chipID == 0) UpdateAY8910State(reg, data);
                else UpdateAY8910State2(reg, data);
            }
            s.in_cmd = false;
        }
    }
}
```

### 集成到 VGMStreamThread

```
VGMStreamThread:
    VgmParseState parseState;
    while running:
        读 VGM 文件到 local buf
        for each byte in buf:
            vgm_parse_byte(parseState, byte)
        rpfm_send_vgm_data(buf, len, &bufLevel)
```

### 注意事项

- UpdateAY8910State 是线程安全的（只写 static 变量，UI 在主线程读）
- 解析开销可忽略
- 仅上位机改动，固件不变

## 文件清单

| 文件 | 改动 |
|------|------|
| `RPFM_Player/src/ay8910_window.cpp` | VGMStreamThread 加解析状态机 + 缓冲区滑块 + 持久化 |

## 验证

1. 缓冲区滑块：调整后播放正常，更小的缓冲区延迟更低
2. 影子寄存器：缓冲模式下钢琴键盘、音高显示、电平表正常更新
3. 切换回实时模式不受影响
