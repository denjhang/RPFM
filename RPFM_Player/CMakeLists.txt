cmake_minimum_required(VERSION 3.16)
project(RPFM_Player CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ImGui
add_library(imgui_lib STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_impl_win32.cpp
    imgui/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PRIVATE ${CMAKE_SOURCE_DIR}/imgui)

# Application
add_executable(rpfm_player WIN32
    src/main.cpp
    src/rpfm_hid.cpp
    src/ay8910_window.cpp
    src/core/vgm_player_ui.cpp
)

target_include_directories(rpfm_player PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/midi
    ${CMAKE_SOURCE_DIR}/src/windows/sn76489
    ${CMAKE_SOURCE_DIR}/src/windows/ym2413
    ${CMAKE_SOURCE_DIR}/src/windows/ym2163
    ${CMAKE_SOURCE_DIR}/src/windows/spfm
    ${CMAKE_SOURCE_DIR}/src/libvgm-modizer/emu/cores
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/imgui
)

target_compile_options(rpfm_player PRIVATE -Wall -Wextra -O2)
target_link_options(rpfm_player PRIVATE -static)

target_link_libraries(rpfm_player PRIVATE
    imgui_lib
    d3d11 dxgi d3dcompiler dwmapi
    setupapi hid ws2_32 gdi32 user32
    winmm
    z
)
