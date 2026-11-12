# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.13)

# Nothing about the chip or the wiring is assumed: the chip, the board and the UART are stated.
#   cmake -S firmware -B build -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico \
#         -DMIMHID_UART_ID=uart0 -DMIMHID_UART_TX_PIN=0 -DMIMHID_UART_RX_PIN=1
#   cmake -S firmware -B build -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 (the same UART arguments)
foreach(required PICO_PLATFORM PICO_BOARD)
    if(NOT DEFINED ${required} AND NOT DEFINED ENV{${required}})
        message(FATAL_ERROR "Set -D${required}=<value>: the build assumes no chip or board")
    endif()
endforeach()
if(NOT DEFINED MIMHID_UART_ID)
    message(FATAL_ERROR "Set -DMIMHID_UART_ID=uart0 or uart1: the build assumes no UART")
endif()
if(NOT DEFINED MIMHID_UART_TX_PIN)
    message(FATAL_ERROR "Set -DMIMHID_UART_TX_PIN=<gpio>: the build assumes no pin")
endif()
if(NOT DEFINED MIMHID_UART_RX_PIN)
    message(FATAL_ERROR "Set -DMIMHID_UART_RX_PIN=<gpio>: the build assumes no pin")
endif()
if(DEFINED PICO_PLATFORM)
    set(mimhid_platform "${PICO_PLATFORM}")
else()
    set(mimhid_platform "$ENV{PICO_PLATFORM}")
endif()
if(NOT mimhid_platform MATCHES "^(rp2040|rp2350|rp2350-arm-s)$")
    message(FATAL_ERROR "PICO_PLATFORM is rp2040, rp2350 or rp2350-arm-s, not '${mimhid_platform}'")
endif()

# Include Pico SDK
include(pico_sdk_import.cmake)

project(mimhid_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The RP2040 and the RP2350 have the endpoint numbers 1 to 15 in both directions (see core.h).
add_executable(mimhid_firmware
    rp/main.c
    rp/hal_rp.c
    common/core.c
    common/control.c
    common/ep0.c
    common/usb_descriptors.c
    common/generic.c
)

target_include_directories(mimhid_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/common
    ${CMAKE_CURRENT_LIST_DIR}/../proto
)

target_compile_definitions(mimhid_firmware PRIVATE
    MIMHID_MAX_ENDPOINT_NUMBER=15
    MIMHID_MAX_ENDPOINT_BYTES=64
    # Isochronous endpoints: TinyUSB's RP2040 driver has dcd_edpt_iso_alloc and dcd_edpt_iso_activate.
    # Full speed takes 1023 bytes a frame (USB 2.0 5.6.3). The driver's packet memory is the 4 KiB
    # DPRAM less 0x180 bytes of registers, and it asserts that one byte is left (dcd_rp2040.c,
    # hw_endpoint_alloc): 3712 bytes, of which 64-byte units fit up to 3648.
    MIMHID_ISO_BYTES=1023
    MIMHID_EP_BUFFER_BYTES=3648
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
    MIMHID_UART_ID=${MIMHID_UART_ID}
    MIMHID_UART_TX_PIN=${MIMHID_UART_TX_PIN}
    MIMHID_UART_RX_PIN=${MIMHID_UART_RX_PIN}
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
target_link_options(mimhid_firmware PRIVATE
    -Wl,--wrap=dcd_event_handler
    -Wl,--wrap=dcd_edpt_stall
    -Wl,--wrap=dcd_edpt_xfer
)

pico_enable_stdio_usb(mimhid_firmware 0)
pico_enable_stdio_uart(mimhid_firmware 0)

pico_add_extra_outputs(mimhid_firmware)
