#!/bin/bash
# flash.sh - RPFM 自动烧录（HID 版本）
# 通过 HID 发送 BOOTSEL 命令让 RP2350A 进入 BOOTSEL 模式，然后复制 UF2 烧录
#
# 用法: bash flash.sh [UF2文件]

set -e

UF2_FILE="${1:-build_rpfm/rpfm.uf2}"

if [ ! -f "$UF2_FILE" ]; then
    echo "UF2 not found: $UF2_FILE"
    exit 1
fi

echo "=== RPFM Flash (HID) ==="
echo "Firmware: $UF2_FILE"

# Step 1: Send BOOTSEL via HID test tool
echo "[1/3] Sending BOOTSEL via HID..."
if [ -f test/hid_test.exe ]; then
    # hid_test.exe has a 'bootsel' command
    test/hid_test.exe bootsel 2>&1 || echo "  (device may already be in BOOTSEL)"
else
    echo "  hid_test.exe not found, skip HID BOOTSEL"
fi

# Step 2: Wait for RP2350 drive to appear
echo "[2/3] Waiting for RP2350 drive..."
for i in $(seq 1 30); do
    DRIVE=$(powershell -NoProfile -Command "(Get-Volume | Where-Object { \$_.FileSystemLabel -match '^RP' }).DriveLetter" 2>/dev/null)
    if [ -n "$DRIVE" ]; then
        echo "  Found drive: ${DRIVE}:"
        break
    fi
    sleep 1
done

if [ -z "$DRIVE" ]; then
    echo "ERROR: RP2350 drive not found in 30s"
    echo "Try: hold BOOTSEL button and replug USB"
    exit 1
fi

# Step 3: Copy UF2
echo "[3/3] Flashing..."
cp "$UF2_FILE" "${DRIVE}:/" 2>&1
if [ $? -eq 0 ]; then
    echo ""
    echo "Flash SUCCESS! RP2350A will reboot with new firmware."
else
    echo "Flash FAILED"
    exit 1
fi
