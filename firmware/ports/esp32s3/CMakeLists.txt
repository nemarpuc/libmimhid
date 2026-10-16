# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.16)

# ESP32-S3 firmware (ESP-IDF 5.3 or later).
#   export TINYUSB_PATH=<tinyusb checkout>
#   idf.py -C firmware/ports/esp32s3 set-target esp32s3 build
# Pass -DMIMHID_UART_TX_PIN=<gpio> / -DMIMHID_UART_RX_PIN=<gpio> to change the UART pins.
if(NOT DEFINED ENV{TINYUSB_PATH})
    message(FATAL_ERROR "Set the TINYUSB_PATH environment variable to a TinyUSB checkout")
endif()

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimhid_firmware_esp32s3)
