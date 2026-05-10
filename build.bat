@echo off
setlocal

:: Use Pico official cmake/ninja, but point to SDK 2.2.0 for RP2350 support
set PATH=D:\Program Files\Raspberry Pi\Pico SDK v1.5.1\cmake\bin;D:\Program Files\Raspberry Pi\Pico SDK v1.5.1\gcc-arm-none-eabi\bin;D:\Program Files\Raspberry Pi\Pico SDK v1.5.1\ninja;%PATH%

set PICO_SDK_PATH=D:\working\vscode-projects\YM2163-Midi\RPFM\pico-sdk-2.2.0
set PICO_PLATFORM=rp2350
set PICO_BOARD=pico2

cd /d D:\working\vscode-projects\YM2163-Midi\RPFM

if exist build rmdir /s /q build
mkdir build
cd build

cmake -G Ninja ..
if %errorlevel% neq 0 (
    echo.
    echo === CMake FAILED ===
    pause
    exit /b 1
)

echo.
echo === Building... ===
ninja
if %errorlevel% neq 0 (
    echo.
    echo === Build FAILED ===
    pause
    exit /b 1
)

echo.
echo === Build SUCCESS ===
echo.
dir *.uf2
echo.
pause
