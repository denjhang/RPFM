#!/bin/bash
# flash.sh - RPFM 自动烧录脚本
# 通过 USB CDC 发送 BOOTSEL 命令让 RP2350A 进入 BOOTSEL 模式，然后复制 UF2 烧录
#
# 用法: bash flash.sh [COM端口] [UF2文件]
#   默认: bash flash.sh COM10 build/rpfm_ym2413_test.uf2

set -e

COM_PORT="${1:-COM10}"
UF2_FILE="${2:-build/rpfm_ym2413_test.uf2}"

# 检查 UF2 文件
if [ ! -f "$UF2_FILE" ]; then
    echo "错误: UF2 文件不存在: $UF2_FILE"
    echo "请先编译: bash build.sh"
    exit 1
fi

# 检查 picotool
if ! command -v picotool &>/dev/null; then
    echo "警告: picotool 未找到。将使用复制 UF2 方式烧录。"
fi

echo "=== RPFM 自动烧录 ==="
echo "串口: $COM_PORT"
echo "固件: $UF2_FILE"
echo ""

# Step 1: 持续发送 BOOTSEL 直到 COM 口消失（设备已进入 BOOTSEL）
echo "[1/3] 发送 BOOTSEL 命令..."
python3 -c "
import serial, sys, time

port = '${COM_PORT}'
sent = False

while True:
    try:
        s = serial.Serial(port, 115200, timeout=0.5)
        if not sent:
            print('  已连接，发送 BOOTSEL...')
        else:
            print('  仍在运行，再次发送...')
        s.reset_input_buffer()
        s.write(b'BOOTSEL\r\n')
        s.flush()
        sent = True
        time.sleep(0.2)
        s.close()
    except Exception:
        if sent:
            print('  COM 口已消失，设备进入 BOOTSEL 模式')
            break
        else:
            print(f'  无法打开 {port}，设备可能不在运行')
            sys.exit(1)
"

# Step 2: 等待 RP2350 盘符出现
echo "[2/3] 等待 RP2350 盘符..."
for i in $(seq 1 30); do
    DRIVE=$(powershell -NoProfile -Command "(Get-Volume | Where-Object { \$_.FileSystemLabel -match '^RP' }).DriveLetter" 2>/dev/null)
    if [ -n "$DRIVE" ]; then
        echo "  发现盘符: ${DRIVE}:"
        break
    fi
    sleep 1
done

if [ -z "$DRIVE" ]; then
    echo "错误: 30 秒内未发现 RP2350 盘符"
    echo "手动补救: 按住 BOOTSEL 按钮后重新插拔 USB"
    exit 1
fi

# Step 3: 复制 UF2 烧录
echo "[3/3] 烧录中..."
cp "$UF2_FILE" "${DRIVE}:/" 2>&1
if [ $? -eq 0 ]; then
    echo ""
    echo "烧录成功！RP2350A 将自动重启运行新固件。"
else
    echo ""
    echo "烧录失败，请检查盘符是否可写"
    exit 1
fi
