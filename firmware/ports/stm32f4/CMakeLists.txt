# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.17)

# STM32F4 firmware built on the TinyUSB board support package.
#   cmake -S firmware/ports/stm32f4 -B build -DTINYUSB_PATH=<tinyusb> [-DBOARD=stm32f411blackpill]
# Fetch the BSP dependencies first: python3 <tinyusb>/tools/get_deps.py stm32f4
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()
# The board is the chip: it fixes the pins and the endpoints, so it is never picked for the user.
if(NOT BOARD)
    message(FATAL_ERROR "Set -DBOARD=<board of the TinyUSB BSP>, e.g. stm32f411blackpill")
endif()
# IN endpoints of the full-speed OTG core, without endpoint 0 (USB_OTG_FS_MAX_IN_ENDPOINTS of the
# CMSIS device header, which counts endpoint 0: 4 on the F401, F405, F407, F411, F429 and F439, 6 on
# the F412, F413, F423, F446, F469 and F479). Each interface takes one IN endpoint (see core.h).
if(BOARD MATCHES "^(stm32f401blackpill|stm32f407blackvet|stm32f407disco|stm32f411blackpill|stm32f411disco|stm32f439nucleo|feather_stm32f405|pyboardv11)$")
    set(MIMUSB_STM32_ENDPOINTS 3)
elseif(BOARD MATCHES "^(stm32f412disco|stm32f412nucleo)$")
    set(MIMUSB_STM32_ENDPOINTS 5)
else()
    message(FATAL_ERROR "BOARD '${BOARD}': its MCU's USB_OTG_FS_MAX_IN_ENDPOINTS is not recorded in this file; add it before building")
endif()

include(${TINYUSB_PATH}/hw/bsp/family_support.cmake)
project(mimusb_firmware_stm32f4 C CXX ASM)
family_initialize_project(mimusb_firmware_stm32f4 ${CMAKE_CURRENT_LIST_DIR})

add_executable(mimusb_firmware_stm32f4)
target_sources(mimusb_firmware_stm32f4 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/hal_stm32f4.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/core.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/control.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/ep0.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/generic.c
)
target_include_directories(mimusb_firmware_stm32f4 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)

# The endpoint numbers are 1 to what the core has IN endpoints, in both directions (see core.h). The BSP's UART (USART2) is reused,
# starting at the protocol's boot rate.
set(MIMUSB_DEFS
    MIMUSB_MAX_ENDPOINT_NUMBER=${MIMUSB_STM32_ENDPOINTS}
    MIMUSB_MAX_ENDPOINT_BYTES=64
    CFG_TUSB_OS=OPT_OS_NONE
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
    CFG_BOARD_UART_BAUDRATE=115200)

family_configure_device_example(mimusb_firmware_stm32f4 noos)
# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimusb_firmware_stm32f4 PUBLIC -Wl,--wrap=dcd_event_handler -Wl,--wrap=dcd_edpt_stall -Wl,--wrap=dcd_edpt_xfer)
target_include_directories(mimusb_firmware_stm32f4-tinyusb_config INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)
target_compile_definitions(mimusb_firmware_stm32f4 PUBLIC ${MIMUSB_DEFS})
target_compile_definitions(mimusb_firmware_stm32f4-tinyusb_config INTERFACE ${MIMUSB_DEFS})
