# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.16)

# ESP32-S3 firmware (ESP-IDF 5.3 or later).
#   export TINYUSB_PATH=<tinyusb checkout>
#   idf.py -C firmware/ports/esp32s3 set-target esp32s3 build
# Both UART pins are required: -DMIMUSB_UART_TX_PIN=<gpio> -DMIMUSB_UART_RX_PIN=<gpio>
# (idf.py -C firmware/ports/esp32s3 -DMIMUSB_UART_TX_PIN=17 -DMIMUSB_UART_RX_PIN=18 build).
if(NOT DEFINED ENV{TINYUSB_PATH})
    message(FATAL_ERROR "Set the TINYUSB_PATH environment variable to a TinyUSB checkout")
endif()

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimusb_firmware_esp32s3)
