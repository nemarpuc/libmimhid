# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.17)

# nRF52840 firmware built on the TinyUSB board support package.
#   cmake -S firmware/ports/nrf52840 -B build -DTINYUSB_PATH=<tinyusb> [-DBOARD=pca10056]
# Fetch the BSP dependencies first: python3 <tinyusb>/tools/get_deps.py nrf
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()
if(NOT BOARD)
    set(BOARD pca10056)
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
    ${CMAKE_CURRENT_LIST_DIR}/../../common/usb_descriptors.c
)
target_include_directories(mimhid_firmware_nrf52840 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)

family_configure_device_example(mimhid_firmware_nrf52840 noos)
# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_options(mimhid_firmware_nrf52840 PUBLIC -Wl,--wrap=dcd_event_handler -Wl,--wrap=dcd_edpt_stall)
target_include_directories(mimhid_firmware_nrf52840-tinyusb_config INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_LIST_DIR}/../../../proto)
