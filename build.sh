#!/bin/bash
# build.sh - Build RPFM firmware under MSYS2/mingw64
set -e

cd "$(dirname "$0")"

SDK_PATH="$(pwd)/pico-sdk-2.2.0"
BUILD_DIR="build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Configuring ==="
echo "Using Unix Makefiles"
PICO_SDK_PATH="$SDK_PATH" cmake -G "Unix Makefiles" \
    -DPICO_PLATFORM=rp2350 \
    -DPICO_BOARD=pico2 \
    -DPICO_CONFIG_HEADER_FILE="${SDK_PATH}/../pico_sdk_config.h" \
    ..
BUILD_CMD="make -j4"

echo "=== Fixing MSYS2 path issue ==="
AUTOGEN="generated/pico_base/pico/config_autogen.h"
if [ -f "$AUTOGEN" ]; then
    sed -i 's|"/d/|"D:/|g' "$AUTOGEN"
    sed -i 's|"/c/|"C:/|g' "$AUTOGEN"
    echo "Fixed paths in $AUTOGEN"
    head -8 "$AUTOGEN"
fi

echo "=== Building ==="
$BUILD_CMD

echo ""
echo "=== Build SUCCESS ==="
ls -la *.uf2 2>/dev/null && echo "UF2 file ready!" || echo "No .uf2 found, check build output"
