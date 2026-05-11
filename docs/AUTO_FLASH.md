# RPFM 自动烧录指南

> 记录时间：2026-05-11
> 平台：Windows 11, MSYS2 (mingw64), RP2350A

## 1. 概述

`flash.sh` 实现全自动烧录流程：通过 USB CDC 串口发送命令让 RP2350A 进入 BOOTSEL 模式，然后复制 UF2 文件完成烧录。整个过程无需手动按 BOOTSEL 按钮。

## 2. 前提条件

### 2.1 Python + pyserial

```bash
pacman -S mingw-w64-x86_64-python-pyserial
```

### 2.2 已编译的 UF2 文件

```bash
bash build.sh
```

### 2.3 固件已支持 BOOTSEL 命令

当前 `main.c` 的 `check_serial_command()` 已实现：收到 `BOOTSEL\r\n` 后调用 `rom_reset_usb_boot(0, 0)`。

## 3. 使用方法

```bash
# 默认 COM10 + build/rpfm_ym2413_test.uf2
bash flash.sh

# 指定 COM 口和 UF2 文件
bash flash.sh COM10 build/rpfm_ym2413_test.uf2
```

输出示例：

```
=== RPFM 自动烧录 ===
串口: COM10
固件: build/rpfm_ym2413_test.uf2

[1/3] 发送 BOOTSEL 命令...
  已连接，发送 BOOTSEL...
  仍在运行，再次发送...
  COM 口已消失，设备进入 BOOTSEL 模式
[2/3] 等待 RP2350 盘符...
  发现盘符: G:
[3/3] 烧录中...

烧录成功！RP2350A 将自动重启运行新固件。
```

## 4. 工作流程

```
┌─────────────┐     BOOTSEL\r\n      ┌─────────────┐
│  flash.sh   │ ──────────────────→   │  RP2350A    │
│  (PC 端)    │ ←─── CDC 串口 ────    │  (固件)     │
└─────────────┘                       └──────┬──────┘
                                            │ rom_reset_usb_boot(0,0)
                                            ▼
                                     ┌──────────────┐
                                     │ BOOTSEL 模式 │
                                     │ 出现 U 盘    │
                                     └──────┬───────┘
                                            │
┌─────────────┐    复制 .uf2 到盘符   ┌──────┴───────┐
│  flash.sh   │ ──────────────────→  │  RP2350A    │
│  (PC 端)    │                       │  自动重启    │
└─────────────┘                       └──────────────┘
```

### Step 1: 发送 BOOTSEL 命令

- 用 `pyserial` 打开 COM 口
- 发送 `BOOTSEL\r\n`
- **持续重试**直到 COM 口消失（说明设备已重启进入 BOOTSEL 模式）
- 每次发送间隔 200ms

### Step 2: 等待 RP2350 盘符

- 用 PowerShell `Get-Volume` 扫描磁盘标签以 "RP" 开头的盘符
- 每秒检查一次，最多等待 30 秒

### Step 3: 复制 UF2 烧录

- 执行 `cp rpfm_ym2413_test.uf2 G:/`
- 复制完成后 RP2350A 自动重启运行新固件

## 5. 关键技术细节

### 5.1 为什么需要持续发送？

固件的 `check_serial_command()` 只在音频播放间隙检查串口输入（约每 370ms 一次）。发送一次可能恰好错过检查窗口。持续发送确保不会错过。

### 5.2 为什么不用 picotool？

- `picotool load` 需要 picotool 识别 BOOTSEL 设备，有时识别慢
- 直接复制 UF2 到 RP2350 的 U 盘更可靠，Windows 即插即用

### 5.3 rom_reset_usb_boot vs reset_usb_boot

两者等价，`reset_usb_boot` 是 `rom_reset_usb_boot` 的内联包装。官方示例（nuke.c 等）使用 `reset_usb_boot(0, 0)`。

### 5.4 COM 口消失 = 设备已重启

`rom_reset_usb_boot()` 会让 RP2350A 断开 USB 重新枚举为 Mass Storage 设备。因此 COM 口消失是 BOOTSEL 生效的可靠信号。

## 6. 手动烧录（备用）

如果自动烧录失败，手动操作：

1. 按住 RP2350A 上的 BOOTSEL 按钮
2. 重新插拔 USB
3. 出现 RP2350 U 盘后复制 UF2 文件
4. 松开按钮，设备自动重启

## 7. 踩坑记录

### 坑 1: getchar_timeout_us 放在主循环外层无效

**现象**：发送 BOOTSEL 后音乐仍在播放，设备不重启。

**根因**：`getchar_timeout_us(0)` 只在 `play_melody()` 返回后调用，一个旋律周期约 17 秒。CDC 数据在缓冲区里但一直没被读取。

**解决**：在 `play_melody()`、`play_scale()`、`play_instrument_demo()` 等所有长循环内部，每个音符间隙调用 `check_serial_command()`。

### 坑 2: MSYS2 stty 无法操作 COM 口

**现象**：`stty -F /dev/ttyS9` 报 "unable to perform all requested operations"。

**根因**：MSYS2 的 `stty` 对 Windows COM 口支持有限。

**解决**：改用 Python `pyserial` 库操作串口。

### 坑 3: pyserial COM 口映射

**现象**：`serial.Serial('COM10')` 报 `FileNotFoundError`。

**根因**：设备已断开或 COM 口号不正确。

**解决**：发送 BOOTSEL 后 COM 口会消失（正常），脚本应检测此事件作为成功信号。

## 8. 依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| python3 | MSYS2 自带 | 脚本运行时 |
| pyserial | `pacman -S mingw-w64-x86_64-python-pyserial` | 串口通信 |
| powershell | Windows 自带 | 检测 RP2350 盘符 |
