# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.13)

# Include Pico SDK
include(pico_sdk_import.cmake)

project(mimhid_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# RP2040 default, RP2350 (Pico 2) with -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2.
add_executable(mimhid_firmware
    rp/main.c
    rp/hal_rp.c
    common/core.c
    common/control.c
    common/usb_descriptors.c
)

target_include_directories(mimhid_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/common
    ${CMAKE_CURRENT_LIST_DIR}/../proto
)

target_link_libraries(mimhid_firmware
    pico_stdlib
    pico_multicore
    hardware_uart
    hardware_dma
    pico_unique_id
    tinyusb_device
    tinyusb_board
)

# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimhid_firmware PRIVATE -Wl,--wrap=dcd_event_handler)

pico_enable_stdio_usb(mimhid_firmware 0)
pico_enable_stdio_uart(mimhid_firmware 0)

pico_add_extra_outputs(mimhid_firmware)
