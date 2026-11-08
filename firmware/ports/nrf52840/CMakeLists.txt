# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
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
project(mimhid_firmware_nrf52840 C CXX ASM)
family_initialize_project(mimhid_firmware_nrf52840 ${CMAKE_CURRENT_LIST_DIR})

add_executable(mimhid_firmware_nrf52840)
target_sources(mimhid_firmware_nrf52840 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/hal_nrf52.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/core.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/ep0.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/usb_descriptors.c
)
target_include_directories(mimhid_firmware_nrf52840 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)

# The nRF52840 has the endpoints EPIN1..7 and EPOUT1..7 (number 8 is the isochronous one), so the
# host packs nodes into seven interfaces (see core.h). Full speed only.
set(MIMHID_DEFS
    MIMHID_HID_ITFS=7
    MIMHID_MAX_ENDPOINT_NUMBER=7
    MIMHID_MAX_ENDPOINT_BYTES=64
    CFG_TUSB_OS=OPT_OS_NONE
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED)

family_configure_device_example(mimhid_firmware_nrf52840 noos)
# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimhid_firmware_nrf52840 PUBLIC -Wl,--wrap=dcd_event_handler -Wl,--wrap=dcd_edpt_stall -Wl,--wrap=dcd_edpt_xfer -Wl,--wrap=tusb_hal_nrf_power_event)
target_include_directories(mimhid_firmware_nrf52840-tinyusb_config INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)
target_compile_definitions(mimhid_firmware_nrf52840 PUBLIC ${MIMHID_DEFS})
target_compile_definitions(mimhid_firmware_nrf52840-tinyusb_config INTERFACE ${MIMHID_DEFS})
