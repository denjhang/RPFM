cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Use local SDK if PICO_SDK_PATH not already set
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    get_filename_component(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pico-sdk-2.2.0" ABSOLUTE)
    message("Using local PICO_SDK_PATH: ${PICO_SDK_PATH}")
endif()

include(pico_sdk_import.cmake)

project(rpfm C CXX ASM)

pico_sdk_init()

# ========== Protocol Selection ==========
# Set PROTOCOL to "scci" or "rpfm" (default: rpfm)
set(PROTOCOL "rpfm" CACHE STRING "Protocol: scci or rpfm")

if(PROTOCOL STREQUAL "scci")
    set(PROTO_DIR "scci")
    set(TARGET_NAME "rpfm_scci")
else()
    set(PROTO_DIR "rpfm")
    set(TARGET_NAME "rpfm")
endif()

message("Building protocol: ${PROTO_DIR} -> ${TARGET_NAME}")

add_executable(${TARGET_NAME}
    src/${PROTO_DIR}/main.c
)

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/${PROTO_DIR}/ym2413.pio)
pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/${PROTO_DIR}/pio_cs.pio)
pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/${PROTO_DIR}/ws2812.pio)

target_link_libraries(${TARGET_NAME} PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_pio
    pico_multicore
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
)

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
