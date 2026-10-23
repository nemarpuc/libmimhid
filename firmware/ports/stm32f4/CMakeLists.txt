# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.17)

# STM32F4 firmware built on the TinyUSB board support package.
#   cmake -S firmware/ports/stm32f4 -B build -DTINYUSB_PATH=<tinyusb> [-DBOARD=stm32f411blackpill]
# Fetch the BSP dependencies first: python3 <tinyusb>/tools/get_deps.py stm32f4
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()
if(NOT BOARD)
    set(BOARD stm32f411blackpill)
endif()

include(${TINYUSB_PATH}/hw/bsp/family_support.cmake)
project(mimhid_firmware_stm32f4 C CXX ASM)
family_initialize_project(mimhid_firmware_stm32f4 ${CMAKE_CURRENT_LIST_DIR})

add_executable(mimhid_firmware_stm32f4)
target_sources(mimhid_firmware_stm32f4 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/hal_stm32f4.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/core.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/usb_descriptors.c
)
target_include_directories(mimhid_firmware_stm32f4 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)

# The full-speed OTG core has three usable IN endpoints, so the host packs nodes into three
# interfaces (see core.h).
# The BSP's UART (USART2) is reused, starting at the protocol's boot rate.
set(MIMHID_DEFS MIMHID_HID_ITFS=3 CFG_BOARD_UART_BAUDRATE=115200)

family_configure_device_example(mimhid_firmware_stm32f4 noos)
# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimhid_firmware_stm32f4 PUBLIC -Wl,--wrap=dcd_event_handler -Wl,--wrap=dcd_edpt_stall)
target_include_directories(mimhid_firmware_stm32f4-tinyusb_config INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)
target_compile_definitions(mimhid_firmware_stm32f4 PUBLIC ${MIMHID_DEFS})
target_compile_definitions(mimhid_firmware_stm32f4-tinyusb_config INTERFACE ${MIMHID_DEFS})
