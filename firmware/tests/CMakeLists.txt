# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.13)
project(mimhid_firmware_tests C)
enable_testing()

# Host test of the portable firmware core. Configure with -DTINYUSB_PATH=<tinyusb checkout>.
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()

# What a port's build states (core.h, tusb_config.h): its interfaces, its highest endpoint number
# and the speed. The numbers are the ports': 15 and 15 (RP2040, RP2350), 7 and 7 (nRF52840), 4 and 6
# (ESP32-S3), 3 and 3 (STM32F4).
function(mimhid_port_defs itfs out)
    if(itfs EQUAL 4)
        set(number 6)
    else()
        set(number ${itfs})
    endif()
    set(${out}
        MIMHID_HID_ITFS=${itfs}
        MIMHID_MAX_ENDPOINT_NUMBER=${number}
        MIMHID_MAX_ENDPOINT_BYTES=64
        CFG_TUSB_OS=OPT_OS_NONE
        "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
        CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
        PARENT_SCOPE)
endfunction()

foreach(itfs 15 7 4 3)
    mimhid_port_defs(${itfs} port_defs)
    add_executable(test_core_${itfs}
        test_core.c
        ../common/core.c
        ../common/control.c
        ../common/ep0.c
        ../common/usb_descriptors.c
        ../common/generic.c
    )
    target_include_directories(test_core_${itfs} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}      # test tusb_config.h
        ../common
        ../../proto
        ${TINYUSB_PATH}/src
    )
    target_compile_definitions(test_core_${itfs} PRIVATE
        ${port_defs}
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
    ../common/generic.c
)
target_include_directories(test_core_hs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../common
    ../../proto
    ${TINYUSB_PATH}/src
)
target_compile_definitions(test_core_hs PRIVATE
    MIMHID_HID_ITFS=7
    MIMHID_MAX_ENDPOINT_NUMBER=7
    MIMHID_MAX_ENDPOINT_BYTES=1024
    CFG_TUSB_OS=OPT_OS_NONE
    CFG_TUSB_CONFIG_FILE="tusb_config.h"
    CFG_TUD_MAX_SPEED=OPT_MODE_HIGH_SPEED
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_HIGH_SPEED)"
)
target_compile_options(test_core_hs PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME firmware_core_hs COMMAND test_core_hs)

# What the port states must come out the same in every source, whichever header it reads first (an
# interface count that differs between core.c and usb_descriptors.c overruns arrays).
foreach(order first_core first_tusb)
    foreach(itfs 15 7 4 3)
        mimhid_port_defs(${itfs} port_defs)
        set(name itfs_${order}_${itfs})
        add_executable(${name} itfs_probe.c)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ../common ../../proto ${TINYUSB_PATH}/src)
        target_compile_definitions(${name} PRIVATE
            CFG_TUSB_CONFIG_FILE="tusb_config.h" CFG_TUSB_MCU=OPT_MCU_RP2040 ${port_defs})
        if(order STREQUAL "first_tusb")
            target_compile_definitions(${name} PRIVATE ITFS_TUSB_FIRST)
        endif()
        if(itfs EQUAL 4)
            set(number 6)
        else()
            set(number ${itfs})
        endif()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "^${itfs} ${number} 64\n")
    endforeach()
endforeach()

# A build that leaves one of them out must stop, with the message that names it: the compiler is run
# on the probe with the others given.
foreach(missing MIMHID_HID_ITFS MIMHID_MAX_ENDPOINT_NUMBER MIMHID_MAX_ENDPOINT_BYTES
                CFG_TUSB_OS CFG_TUSB_RHPORT0_MODE CFG_TUD_MAX_SPEED)
    mimhid_port_defs(7 port_defs)
    set(flags "")
    foreach(def IN LISTS port_defs)
        string(REGEX REPLACE "=.*$" "" def_name "${def}")
        string(REGEX REPLACE "^\"" "" def_name "${def_name}")
        if(NOT def_name STREQUAL missing)
            list(APPEND flags "-D${def}")
        endif()
    endforeach()
    add_test(NAME missing_${missing}
        COMMAND ${CMAKE_C_COMPILER} -fsyntax-only ${flags}
            -DCFG_TUSB_CONFIG_FILE="tusb_config.h" -DCFG_TUSB_MCU=OPT_MCU_RP2040
            -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/../common
            -I${CMAKE_CURRENT_SOURCE_DIR}/../../proto -I${TINYUSB_PATH}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/itfs_probe.c)
    set_tests_properties(missing_${missing} PROPERTIES
        PASS_REGULAR_EXPRESSION "${missing} must be defined|MIMHID_[A-Z_]+ must be defined")
endforeach()
