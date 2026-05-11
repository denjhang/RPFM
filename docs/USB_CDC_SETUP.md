# RPFM USB CDC 配置指南

> 记录时间：2026-05-11
> 硬件：RP2350A 淘宝核心板，Windows 11
> 环境：MSYS2 mingw64，Pico SDK 2.2.0，arm-none-eabi-gcc 13.3.0

## 1. 概述

USB CDC（Communications Device Class）允许 RP2350A 通过 USB 模拟一个串口，`printf()` 输出直接在电脑上通过串口调试工具查看，无需额外 UART 硬件。

## 2. 前提条件

### 2.1 替换 TinyUSB（必须）

SDK 2.2.0 内置的 TinyUSB 在 RP2350 上调用 `tusb_init()` 时会触发 HardFault 崩溃（`TU_ASSERT` 失败 → `bkpt` 指令）。这是 TinyUSB 与 RP2350 USB 控制器的兼容性问题，与 GitHub Issue #3491 / #3495 报告的问题一致。

**必须替换为 TinyUSB 0.20.0：**

```bash
cd pico-sdk-2.2.0/lib
mv tinyusb tinyusb_backup       # 备份原版
cp -r ../../tinyusb-0.20.0 tinyusb  # 复制新版
```

替换后需重新 cmake 配置和编译。

### 2.2 确认 TinyUSB 可用

cmake 配置时应看到：

```
TinyUSB available at .../pico-sdk-2.2.0/lib/tinyusb/hw/bsp/rp2040; enabling build support for USB.
```

如果看到 "TinyUSB submodule has not been initialized"，说明 TinyUSB 文件不完整。

## 3. CMakeLists.txt 配置

### 3.1 正确配置

```cmake
target_link_libraries(target PRIVATE pico_stdlib hardware_gpio hardware_pio)

pico_enable_stdio_usb(target 1)
pico_enable_stdio_uart(target 0)
```

只链接 `pico_stdlib`，不要手动链接 `tinyusb_device`。SDK 的 `pico_enable_stdio_usb()` 会自动处理所有 TinyUSB 的链接、配置和初始化。

参考：官方 `pico-examples/hello_world/usb/hello_usb` 示例。

### 3.2 错误配置：手动链接 tinyusb_device

```cmake
# 错误！不要这样做
target_link_libraries(target PRIVATE pico_stdlib tinyusb_device)
```

**后果**：

SDK 设置 `LIB_TINYUSB_DEVICE` 标志后，`pico_stdio_usb/include/tusb_config.h` 中的条件判断被跳过：

```c
// pico_stdio_usb/include/tusb_config.h (第32行)
#if !defined(LIB_TINYUSB_HOST) && !defined(LIB_TINYUSB_DEVICE)
#define CFG_TUD_CDC  (1)  // 只有这里才会启用 CDC
#endif
```

`CFG_TUD_CDC` 不会被定义 → CDC 类未启用 → USB 描述符里没有 CDC 接口 → `stdio_usb_init()` 返回 false → 固件不挂但 USB 不工作。

## 4. 固件代码

```c
#include <stdio.h>
#include "pico/stdlib.h"

int main() {
    stdio_init_all();  // 内部自动初始化 TinyUSB 和 USB CDC

    // stdio_init_all() 不会阻塞
    // 即使 USB 未连接，printf() 也会继续执行
    // 数据会被丢弃直到 USB 连接建立

    while (true) {
        printf("Hello %d\n", count++);
        sleep_ms(1000);
    }
}
```

### 4.1 stdio_init_all() 行为

- 内部调用 `stdio_usb_init()` → `tusb_init()` + USB IRQ 注册
- 不等待 USB 连接（默认 `PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0`）
- 返回后 `printf()` 即可使用（USB 未连接时数据静默丢弃）
- USB 连接建立后 `printf()` 输出自动通过 CDC 传输

### 4.2 等待 USB 连接（可选）

如果需要等待 USB 连接后再继续：

```cmake
# CMakeLists.txt 中添加
target_compile_definitions(target PRIVATE
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)
```

设置超时 3 秒，在此期间等待 USB 连接。超时后继续执行。

## 5. 串口监视器

### 5.1 编译

```bash
gcc -o rpfm_monitor.exe rpfm_monitor.c -mwindows -lsetupapi
```

需要 `libsetupapi.a`（MSYS2 mingw64 自带）。

### 5.2 使用方法

1. 烧录固件，连接 USB
2. 等待 Windows 识别 USB 设备（设备管理器中出现 COM 口）
3. 打开 rpfm_monitor.exe
4. 点 "Scan" 刷新端口列表
5. 选择 COM 口，点 "Connect"
6. 查看 printf 输出

### 5.3 注意事项

- 打开串口时使用 `GENERIC_READ | GENERIC_WRITE`（`CreateFileA` 参数），CDC 设备需要读写权限
- COM 口可能需要等固件启动后才会出现
- 如果 "cant open" error 2，先点 Scan 刷新端口列表

## 6. 踩坑排查

### 6.1 固件不启动（LED 常亮/不闪）

**可能原因**：

| 现象 | 原因 | 解决 |
|------|------|------|
| LED 完全不亮 | 固件未烧录成功 | 重新按住 BOOTSEL 拖放 .uf2 |
| LED 3 次快闪后常亮 | `tusb_init()` 崩溃 | 确认已替换 TinyUSB 为 0.20.0 |
| LED 慢闪但无 USB | TinyUSB 初始化成功但 USB 枚举失败 | 检查 USB 线、尝试换 USB 口 |

### 6.2 USB 设备出现但无 COM 口

**可能原因**：

| 现象 | 原因 | 解决 |
|------|------|------|
| 设备管理器有"未知设备" | USB 描述符错误 | 确认没有手动链接 `tinyusb_device` |
| 设备管理器有 USB 设备但无 COM | `CFG_TUD_CDC` 未定义 | 检查编译标志中是否有 `LIB_TINYUSB_DEVICE` |
| COM 口短暂出现后消失 | 固件 HardFault | 确认 TinyUSB 版本 |

### 6.3 COM 口存在但打开失败 error 2

**原因**：`ERROR_FILE_NOT_FOUND`，COM 口在打开时已消失。

**解决**：先 Scan 刷新端口列表，确认 COM 口仍存在再 Connect。

### 6.4 能连接但无数据输出

**原因**：USB 连接后 printf 数据缓冲区未刷新。

**解决**：这是正常的，`stdio_usb` 会自动 flush。如果长时间无数据，检查固件是否真的在调用 printf。

## 7. 完整工作示例

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)

if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    get_filename_component(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pico-sdk-2.2.0" ABSOLUTE)
endif()

include(pico_sdk_import.cmake)
project(my_usb_project C CXX ASM)
pico_sdk_init()

add_executable(my_usb_project main.c)
target_link_libraries(my_usb_project pico_stdlib hardware_gpio)
pico_enable_stdio_usb(my_usb_project 1)
pico_enable_stdio_uart(my_usb_project 0)
pico_add_extra_outputs(my_usb_project)
```

### main.c

```c
#include <stdio.h>
#include "pico/stdlib.h"

int main() {
    stdio_init_all();

    while (true) {
        printf("Hello, USB CDC!\n");
        sleep_ms(1000);
    }
}
```

### 编译烧录

```bash
mkdir build && cd build
cmake -G "Unix Makefiles" -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
# 修复 MSYS2 路径问题
sed -i 's|"/d/|"D:/|g' generated/pico_base/pico/config_autogen.h
make -j4
# 烧录 build/my_usb_project.uf2
```

## 8. 参考资料

| 资源 | 说明 |
|------|------|
| pico-examples/hello_world/usb/hello_usb.c | 官方 USB CDC 最简示例 |
| pico-sdk-2.2.0/src/rp2_common/pico_stdio_usb/ | SDK USB CDC 实现 |
| TinyUSB GitHub Issue #3491, #3495 | RP2350 USB 崩溃 bug 报告 |
