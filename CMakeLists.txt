cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Use local SDK if PICO_SDK_PATH not already set
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    get_filename_component(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pico-sdk-2.2.0" ABSOLUTE)
    message("Using local PICO_SDK_PATH: ${PICO_SDK_PATH}")
endif()

include(pico_sdk_import.cmake)

# Custom config: disable vendor reset interface for Windows CDC compatibility
list(APPEND PICO_CONFIG_HEADER_FILES ${CMAKE_CURRENT_SOURCE_DIR}/pico_sdk_config.h)

project(rpfm_ym2413_test C CXX ASM)

pico_sdk_init()

add_executable(rpfm_ym2413_test
    main.c
)

pico_generate_pio_header(rpfm_ym2413_test ${CMAKE_CURRENT_SOURCE_DIR}/ym2413.pio)

target_link_libraries(rpfm_ym2413_test PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_pio
)

pico_enable_stdio_usb(rpfm_ym2413_test 1)
pico_enable_stdio_uart(rpfm_ym2413_test 0)

pico_add_extra_outputs(rpfm_ym2413_test)
