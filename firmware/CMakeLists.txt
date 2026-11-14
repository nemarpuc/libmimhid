# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.13)

# Nothing about the chip or the wiring is assumed: the chip, the board and the UART are stated.
#   cmake -S firmware -B build -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico \
#         -DMIMUSB_UART_ID=uart0 -DMIMUSB_UART_TX_PIN=0 -DMIMUSB_UART_RX_PIN=1
#   cmake -S firmware -B build -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 (the same UART arguments)
foreach(required PICO_PLATFORM PICO_BOARD)
    if(NOT DEFINED ${required} AND NOT DEFINED ENV{${required}})
        message(FATAL_ERROR "Set -D${required}=<value>: the build assumes no chip or board")
    endif()
endforeach()
if(NOT DEFINED MIMUSB_UART_ID)
    message(FATAL_ERROR "Set -DMIMUSB_UART_ID=uart0 or uart1: the build assumes no UART")
endif()
if(NOT DEFINED MIMUSB_UART_TX_PIN)
    message(FATAL_ERROR "Set -DMIMUSB_UART_TX_PIN=<gpio>: the build assumes no pin")
endif()
if(NOT DEFINED MIMUSB_UART_RX_PIN)
    message(FATAL_ERROR "Set -DMIMUSB_UART_RX_PIN=<gpio>: the build assumes no pin")
endif()
if(DEFINED PICO_PLATFORM)
    set(mimusb_platform "${PICO_PLATFORM}")
else()
    set(mimusb_platform "$ENV{PICO_PLATFORM}")
endif()
if(NOT mimusb_platform MATCHES "^(rp2040|rp2350|rp2350-arm-s)$")
    message(FATAL_ERROR "PICO_PLATFORM is rp2040, rp2350 or rp2350-arm-s, not '${mimusb_platform}'")
endif()

# Include Pico SDK
include(pico_sdk_import.cmake)

project(mimusb_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The RP2040 and the RP2350 have the endpoint numbers 1 to 15 in both directions (see core.h).
add_executable(mimusb_firmware
    rp/main.c
    rp/hal_rp.c
    common/core.c
    common/control.c
    common/ep0.c
    common/usb_descriptors.c
    common/generic.c
    common/tx.c
)

# The limits of the chip's RAM (common/config.h reads mimusb_limits.h and has no default).
# RAM sizes: RP2040 264 kB = 270336 bytes, RP2350 520 kB = 532480 bytes (the datasheets; the files say
# where). The build stops when fewer than the reserve are left after the image (tools/check_ram.py).
if(mimusb_platform STREQUAL "rp2040")
    set(mimusb_limits_dir rp2040)
    set(mimusb_ram_bytes 270336)
    set(mimusb_ram_reserve 16384)
else()
    set(mimusb_limits_dir rp2350)
    set(mimusb_ram_bytes 532480)
    set(mimusb_ram_reserve 32768)
endif()

target_include_directories(mimusb_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/common
    ${CMAKE_CURRENT_LIST_DIR}/rp/limits/${mimusb_limits_dir}
    ${CMAKE_CURRENT_LIST_DIR}/../proto
)

target_compile_definitions(mimusb_firmware PRIVATE
    MIMUSB_MAX_ENDPOINT_NUMBER=15
    MIMUSB_MAX_ENDPOINT_BYTES=64
    # Isochronous endpoints: TinyUSB's RP2040 driver has dcd_edpt_iso_alloc and dcd_edpt_iso_activate.
    # Full speed takes 1023 bytes a frame (USB 2.0 5.6.3). The driver's packet memory is the 4 KiB
    # DPRAM less 0x180 bytes of registers, and it asserts that one byte is left (dcd_rp2040.c,
    # hw_endpoint_alloc): 3712 bytes, of which 64-byte units fit up to 3648.
    MIMUSB_ISO_BYTES=1023
    MIMUSB_EP_BUFFER_BYTES=3648
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
    MIMUSB_UART_ID=${MIMUSB_UART_ID}
    MIMUSB_UART_TX_PIN=${MIMUSB_UART_TX_PIN}
    MIMUSB_UART_RX_PIN=${MIMUSB_UART_RX_PIN}
)

target_link_libraries(mimusb_firmware
    pico_stdlib
    pico_multicore
    hardware_uart
    hardware_dma
    pico_unique_id
    tinyusb_device
    tinyusb_board
)

# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimusb_firmware PRIVATE
    -Wl,--wrap=dcd_event_handler
    -Wl,--wrap=dcd_edpt_stall
    -Wl,--wrap=dcd_edpt_xfer
)

pico_enable_stdio_usb(mimusb_firmware 0)
pico_enable_stdio_uart(mimusb_firmware 0)

include(${CMAKE_CURRENT_LIST_DIR}/tools/check_ram.cmake)
mimusb_check_ram(mimusb_firmware ${mimusb_ram_bytes} ${mimusb_ram_reserve})

pico_add_extra_outputs(mimusb_firmware)
