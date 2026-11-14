# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.17)

# nRF52840 firmware built on the TinyUSB board support package.
#   cmake -S firmware/ports/nrf52840 -B build -DTINYUSB_PATH=<tinyusb> [-DBOARD=pca10056]
# Fetch the BSP dependencies first: python3 <tinyusb>/tools/get_deps.py nrf
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()
# The board fixes the UART pins (board.h of the BSP), so it is never picked for the user.
if(NOT BOARD)
    message(FATAL_ERROR "Set -DBOARD=<board of the TinyUSB BSP>, e.g. pca10056")
endif()

include(${TINYUSB_PATH}/hw/bsp/family_support.cmake)
project(mimusb_firmware_nrf52840 C CXX ASM)
family_initialize_project(mimusb_firmware_nrf52840 ${CMAKE_CURRENT_LIST_DIR})

add_executable(mimusb_firmware_nrf52840)
target_sources(mimusb_firmware_nrf52840 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/hal_nrf52.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/core.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/ep0.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/generic.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/tx.c
)
target_include_directories(mimusb_firmware_nrf52840 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/limits
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)

# The nRF52840 has the endpoints EPIN1..7 and EPOUT1..7 (number 8 is the isochronous one; see
# core.h). Full speed only.
set(MIMUSB_DEFS
    MIMUSB_MAX_ENDPOINT_NUMBER=7
    MIMUSB_MAX_ENDPOINT_BYTES=64
    CFG_TUSB_OS=OPT_OS_NONE
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED)

# A board that carries a bootloader in flash: the value that bootloader looks for in GPREGRET to
# stay in it after a reset (0x57 for the UF2 mode of the Adafruit nRF52 bootloader, for example).
# With it the BOOTLOADER command works; without it the board answers that it has none. The value
# is the bootloader's own, so it is never assumed.
if(DEFINED MIMUSB_NRF_BOOTLOADER_GPREGRET)
    list(APPEND MIMUSB_DEFS MIMUSB_NRF_BOOTLOADER_GPREGRET=${MIMUSB_NRF_BOOTLOADER_GPREGRET})
endif()

family_configure_device_example(mimusb_firmware_nrf52840 noos)
# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimusb_firmware_nrf52840 PUBLIC -Wl,--wrap=dcd_event_handler -Wl,--wrap=dcd_edpt_stall -Wl,--wrap=dcd_edpt_xfer -Wl,--wrap=tusb_hal_nrf_power_event)
target_include_directories(mimusb_firmware_nrf52840-tinyusb_config INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/limits
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)
target_compile_definitions(mimusb_firmware_nrf52840 PUBLIC ${MIMUSB_DEFS})
target_compile_definitions(mimusb_firmware_nrf52840-tinyusb_config INTERFACE ${MIMUSB_DEFS})

# The RAM of the chip: 256 kB = 262144 bytes (limits/mimusb_limits.h says where it is from); the build
# stops when fewer than the reserve are left after the image (tools/check_ram.py).
include(${CMAKE_CURRENT_LIST_DIR}/../../tools/check_ram.cmake)
# The BSP startup file keeps a stack of 0x4000 bytes (gcc_startup_nrf52840.S, .stack) outside bss.
mimusb_check_ram(mimusb_firmware_nrf52840 262144 16384 16384)
