# RPFM YM2413 开发环境配置记录

> 记录时间：2026-05-11
> 平台：Windows 11, MSYS2 (mingw64), RP2350A 淘宝核心板, SPFM 总线声卡模块 (YM2413)

## 1. 问题背景

Pico SDK 2.2.0 是第一个支持 RP2350 的版本（官方 VS Code 扩展安装的 1.5.1 只支持 RP2040）。在 MSYS2 环境下编译 RP2350 固件遇到了 **三个兼容性问题**，下面逐一记录解决方案。

## 2. 安装 ARM 交叉编译工具链

MSYS2 pacman 安装（**mingw64 终端**，不是 msys2 终端）：

```bash
pacman -S mingw-w64-x86_64-arm-none-eabi-gcc mingw-w64-x86_64-picotool mingw-w64-x86_64-ninja
```

| 包 | 版本 | 说明 |
|---|---|---|
| arm-none-eabi-gcc | 13.3.0 | 比 Pico 官方的 10.3 更新 |
| picotool | 2.1.1 | UF2 烧录工具 |
| ninja | 1.13.1 | 构建系统（备用，实际用 make） |

## 3. 问题一：CMake Generator 兼容性

**现象**：MSYS2 的 cmake 4.1.2 与 Pico 官方安装的 Windows 原生 ninja 不兼容，报 `missing and no known rule to make file`。

**解决**：使用 `Unix Makefiles` generator 代替 `Ninja`。

```bash
cmake -G "Unix Makefiles" -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
make -j4
```

## 4. 问题二：MSYS2 Unix 路径 vs Windows 原生编译器

**现象**：Pico SDK 的 `generate_config_header.cmake` 会在 `config_autogen.h` 中写入 Unix 风格的绝对路径：

```c
#include "/d/working/vscode-projects/.../pico2.h"
```

但 MSYS2 安装的 `arm-none-eabi-gcc` 是 **Windows 原生的**，不认识 `/d/` 路径，报 `fatal error: No such file or directory`。

**根因**：MSYS2 的 cmake 自动将所有路径转为 MSYS Unix 格式（`/c/`, `/d/`），而 Pico SDK 的 cmake 脚本用 `file(GENERATE ... CONTENT ...)` 直接把 cmake 变量值写入文件，其中包含被转换过的路径。

**解决**：在 `make` 之后、编译之前，用 `sed` 把 autogen 文件中的 Unix 路径替换回 Windows 路径。写了一个 `build.sh` 脚本自动化：

```bash
#!/bin/bash
set -e
cd "$(dirname "$0")"
SDK_PATH="$(pwd)/pico-sdk-2.2.0"
rm -rf build && mkdir build && cd build
PICO_SDK_PATH="$SDK_PATH" cmake -G "Unix Makefiles" -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
# 关键修复：把 /d/ 替换为 D:/
AUTOGEN="generated/pico_base/pico/config_autogen.h"
sed -i 's|"/d/|"D:/|g' "$AUTOGEN"
sed -i 's|"/c/|"C:/|g' "$AUTOGEN"
make -j4
```

## 5. 问题三：PIO 汇编器不支持 GCC 内联汇编

**现象**：`ym2413.pio` 的 C SDK 块中使用了 `tight_loop_contents()` 和 `asm volatile(...)` ，导致 PIO 汇编器（pioasm）报 `syntax error`。

**解决**：移除非标准内联汇编：
- `tight_loop_contents()` → 普通 `while` 空循环
- `asm volatile("nop; nop; ...")` → `busy_wait_us_32(1)`（Pico SDK 提供的忙等待函数）

## 6. 完整构建流程

### 6.1 文件结构

```
RPFM/
├── CMakeLists.txt           # Pico SDK 构建配置
├── build.sh                 # 一键构建脚本（含路径修复）
├── main.c                   # 固件主代码（bitbang + PIO 双驱动）
├── main_bitbang.c           # bitbang 版本备份（已验证可发声）
├── ym2413.pio               # PIO 总线驱动程序
├── rpfm_monitor.c           # Win32 GDI 串口监视器（USB CDC）
├── pico_sdk_config.h        # SDK 自定义配置
└── pico-sdk-2.2.0/          # Pico SDK 2.2.0（含 TinyUSB 子模块）
```

### 6.2 一键构建

在 **MSYS2 mingw64 终端** 中：

```bash
cd D:/working/vscode-projects/YM2163-Midi/RPFM
bash build.sh
```

### 6.3 手动构建（分步）

```bash
cd D:/working/vcode-projects/YM2163-Midi/RPFM
mkdir build && cd build

# 1. 配置
PICO_SDK_PATH="$(pwd)/pico-sdk-2.2.0" \
cmake -G "Unix Makefiles" -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..

# 2. 修复 MSYS2 路径问题
sed -i 's|"/d/|"D:/|g' generated/pico_base/pico/config_autogen.h
sed -i 's|"/c/|"C:/|g' generated/pico_base/pico/config_autogen.h

# 3. 编译
make -j4
```

### 6.4 输出文件

```
build/rpfm_ym2413_test.uf2    # 烧录文件（52KB）
build/rpfm_ym2413_test.elf    # ARM ELF 可执行文件
build/rpfm_ym2413_test.hex    # Intel HEX 格式
build/rpfm_ym2413_test.bin    # 二进制文件
```

## 7. 关键 CMake 参数

| 参数 | 值 | 说明 |
|---|---|---|
| PICO_SDK_PATH | 指向 pico-sdk-2.2.0 目录 | 必须用 SDK 2.2.0+ 才支持 RP2350 |
| PICO_PLATFORM | `rp2350` | 目标平台（默认 rp2040） |
| PICO_BOARD | `pico2` | 板级配置（定义引脚、时钟等） |

## 8. 硬件接线

| RP2350A GPIO | 声卡模块 | 信号 |
|:---:|:---:|:---:|
| GPIO0-7 | D0-D7 | 数据总线 (PIO) |
| GPIO8 | WR# | 写信号 (PIO) |
| GPIO9 | A0 | 地址选择 (CPU) |
| GPIO10 | CS0 | 片选 (CPU) |
| GPIO11 | IC | 复位 (CPU) |
| GPIO12-14 | A1-A3 | 不使用 |
| GND | GND | **必须共地** |

> 注意：GPIO0-8 必须连续（PIO `out pins, 9` 要求）。数据总线固定 GPIO0-7，WR# 紧随其后在 GPIO8，其他信号分散分配。

## 9. USB CDC 调试输出

使用 TinyUSB 通过 USB CDC 实现虚拟串口，`printf()` 输出可通过 rpfm_monitor 查看。

### 9.1 CMakeLists.txt 配置

**关键**：不要手动链接 `tinyusb_device`。只需用 `pico_enable_stdio_usb` 即可，SDK 会自动处理 TinyUSB 的链接和配置。

```cmake
target_link_libraries(target PRIVATE pico_stdlib hardware_gpio hardware_pio)
pico_enable_stdio_usb(target 1)
pico_enable_stdio_uart(target 0)
```

参考了官方 `hello_usb` 示例（pico-examples/hello_world/usb/）。

### 9.2 为什么之前认为 USB CDC 不兼容？

调试过程中曾尝试手动链接 `tinyusb_device`，导致固件"死机"（`stdio_init_all()` 永久挂起），误判为 TinyUSB 不兼容 RP2350。

**根因**：手动 `target_link_libraries(... tinyusb_device)` 会使 SDK 设置 `LIB_TINYUSB_DEVICE` 标志。`pico_stdio_usb` 检测到该标志后，跳过自己的 TinyUSB 配置（包括定义 `CFG_TUD_CDC=1`）。结果 TinyUSB 被编译进去了，但 CDC 类未启用，USB 枚举后没有 CDC 接口，`stdio_init_all()` 等待 CDC 就绪就永远挂住。

**结论**：TinyUSB 完全兼容 RP2350。错误在于多此一举手动链接，打乱了 SDK 内部的配置顺序。正确做法是只链接 `pico_stdlib`，让 `pico_enable_stdio_usb()` 内部处理所有事情。

### 9.3 串口监视器

`rpfm_monitor.c` 是 Win32 GDI 串口监视程序，支持 COM 端口自动扫描、连接/断开、清除。

```bash
gcc -o rpfm_monitor.exe rpfm_monitor.c -mwindows -lsetupapi
```

## 10. 已知限制

- PIO 驱动尚未调通（bitbang 驱动已验证可正常发声）
- 只在 MSYS2 mingw64 环境下验证通过，Windows CMD + Pico 官方 cmake 未测试
- 每次 `cmake` 后都需要执行 sed 路径修复
