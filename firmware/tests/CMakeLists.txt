# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.13)
project(mimhid_firmware_tests C)
enable_testing()

# Host test of the portable firmware core. Configure with -DTINYUSB_PATH=<tinyusb checkout>.
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()

foreach(itfs 15 7 4 3)
    add_executable(test_core_${itfs}
        test_core.c
        ../common/core.c
        ../common/control.c
        ../common/ep0.c
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

# The rules only a high-speed device has (USB 2.0 9.6.1 and 9.6.6): the same sources built for
# high speed, running test_high_speed alone.
add_executable(test_core_hs
    test_core.c
    ../common/core.c
    ../common/control.c
    ../common/ep0.c
    ../common/usb_descriptors.c
)
target_include_directories(test_core_hs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../common
    ../../proto
    ${TINYUSB_PATH}/src
)
target_compile_definitions(test_core_hs PRIVATE
    MIMHID_HID_ITFS=7
    CFG_TUSB_CONFIG_FILE="tusb_config.h"
    CFG_TUD_MAX_SPEED=OPT_MODE_HIGH_SPEED
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_HIGH_SPEED)"
)
target_compile_options(test_core_hs PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME firmware_core_hs COMMAND test_core_hs)

# MIMHID_HID_ITFS must come out the same in every source, whichever header it reads first (an
# interface count that differs between core.c and usb_descriptors.c overruns arrays). 15 on the
# RP2040 and the RP2350 (their endpoint numbers 1 to 15), 7 where the port does not say.
foreach(order first_core first_tusb)
    foreach(chip rp2040 rp2350 other)
        set(name itfs_${order}_${chip})
        add_executable(${name} itfs_probe.c)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ../common ../../proto ${TINYUSB_PATH}/src)
        target_compile_definitions(${name} PRIVATE
            CFG_TUSB_CONFIG_FILE="tusb_config.h" CFG_TUSB_MCU=OPT_MCU_RP2040)
        if(chip STREQUAL "rp2040")
            target_compile_definitions(${name} PRIVATE PICO_RP2040=1)
            set(expected 15)
        elseif(chip STREQUAL "rp2350")
            target_compile_definitions(${name} PRIVATE PICO_RP2350=1)
            set(expected 15)
        else()
            set(expected 7)
        endif()
        if(order STREQUAL "first_tusb")
            target_compile_definitions(${name} PRIVATE ITFS_TUSB_FIRST)
        endif()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "^${expected}\n")
    endforeach()
endforeach()
