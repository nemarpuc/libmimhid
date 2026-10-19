# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.13)
project(mimhid_firmware_tests C)
enable_testing()

# Host test of the portable firmware core. Configure with -DTINYUSB_PATH=<tinyusb checkout>.
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()

foreach(itfs 7 4 3)
    add_executable(test_core_${itfs}
        test_core.c
        ../common/core.c
        ../common/control.c
        ../common/usb_descriptors.c
    )
    target_include_directories(test_core_${itfs} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}      # test tusb_config.h
        ../common
        ../../proto
        ${TINYUSB_PATH}/src
    )
    target_compile_definitions(test_core_${itfs} PRIVATE
        MIMHID_HID_ITFS=${itfs}
        CFG_TUSB_CONFIG_FILE="tusb_config.h"
    )
    target_compile_options(test_core_${itfs} PRIVATE -Wall -Wextra -UNDEBUG)
    add_test(NAME firmware_core_${itfs} COMMAND test_core_${itfs})
endforeach()
