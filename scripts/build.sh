#!/bin/bash
# build.sh - Build RPFM firmware under MSYS2/mingw64
# Usage: build.sh [scci|rpfm]   (default: rpfm)
set -e

cd "$(dirname "$0")/.."

PROTOCOL="${1:-rpfm}"

if [ "$PROTOCOL" != "scci" ] && [ "$PROTOCOL" != "rpfm" ]; then
    echo "Usage: $0 [scci|rpfm]"
    exit 1
fi

SDK_PATH="$(pwd)/pico-sdk-2.2.0"
BUILD_DIR="build_${PROTOCOL}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Configuring (${PROTOCOL}) ==="
PICO_SDK_PATH="$SDK_PATH" cmake -G "Unix Makefiles" \
    -DPROTOCOL="$PROTOCOL" \
    -DPICO_PLATFORM=rp2350 \
    -DPICO_BOARD=pico2 \
    -DPICO_CONFIG_HEADER_FILE="${SDK_PATH}/../pico_sdk_config.h" \
    ..

echo "=== Fixing MSYS2 path issue ==="
AUTOGEN="generated/pico_base/pico/config_autogen.h"
if [ -f "$AUTOGEN" ]; then
    sed -i 's|"/d/|"D:/|g' "$AUTOGEN"
    sed -i 's|"/c/|"C:/|g' "$AUTOGEN"
    echo "Fixed paths in $AUTOGEN"
fi

echo "=== Building ==="
make -j4

echo ""
echo "=== Build SUCCESS (${PROTOCOL}) ==="
ls -la *.uf2 2>/dev/null && echo "UF2 file ready!" || echo "No .uf2 found"
