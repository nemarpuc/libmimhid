# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.13)
project(mimusb_firmware_tests C)
enable_testing()

# Host test of the portable firmware core. Configure with -DTINYUSB_PATH=<tinyusb checkout>.
if(NOT TINYUSB_PATH)
    message(FATAL_ERROR "Set TINYUSB_PATH to a TinyUSB checkout")
endif()

# What a port's build states (core.h, tusb_config.h): its highest endpoint number and the speed. The
# numbers are the ports': 15 (RP2040, RP2350), 7 (nRF52840), 6 (ESP32-S3), 3 (STM32F4).
# The limits of each port (config.h reads mimusb_limits.h from the port's directory): the tests build
# against the real files, so what a port states is what is tested. STM32F4 is the 128 kB one.
set(LIM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(limits_15 ${LIM}/rp/limits/rp2040)
set(limits_7 ${LIM}/ports/nrf52840/limits)
set(limits_6 ${LIM}/ports/esp32s3/main)
set(limits_3 ${LIM}/ports/stm32f4/limits/ram128k)
set(limits_hs ${LIM}/rp/limits/rp2350)

function(mimusb_port_defs number out)
    set(${out}
        MIMUSB_MAX_ENDPOINT_NUMBER=${number}
        MIMUSB_MAX_ENDPOINT_BYTES=64
        CFG_TUSB_OS=OPT_OS_NONE
        "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
        CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
        PARENT_SCOPE)
endfunction()

foreach(itfs 15 7 6 3)
    mimusb_port_defs(${itfs} port_defs)
    add_executable(test_core_${itfs}
        test_core.c
        ../common/core.c
        ../common/control.c
        ../common/ep0.c
        ../common/usb_descriptors.c
        ../common/generic.c
        ../common/tx.c
    )
    target_include_directories(test_core_${itfs} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}      # test tusb_config.h
        ${limits_${itfs}}
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
    ../common/tx.c
)
target_include_directories(test_core_hs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${limits_hs}
    ../common
    ../../proto
    ${TINYUSB_PATH}/src
)
target_compile_definitions(test_core_hs PRIVATE
    MIMUSB_MAX_ENDPOINT_NUMBER=7
    MIMUSB_MAX_ENDPOINT_BYTES=1024
    CFG_TUSB_OS=OPT_OS_NONE
    CFG_TUSB_CONFIG_FILE="tusb_config.h"
    CFG_TUD_MAX_SPEED=OPT_MODE_HIGH_SPEED
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_HIGH_SPEED)"
)
target_compile_options(test_core_hs PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME firmware_core_hs COMMAND test_core_hs)

# What the port states must come out the same in every source, whichever header it reads first (an
# endpoint number limit that differs between core.c and usb_descriptors.c overruns arrays).
foreach(order first_core first_tusb)
    foreach(itfs 15 7 6 3)
        mimusb_port_defs(${itfs} port_defs)
        set(name probe_${order}_${itfs})
        add_executable(${name} port_probe.c)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ../common ../../proto ${TINYUSB_PATH}/src)
        target_compile_definitions(${name} PRIVATE
            CFG_TUSB_CONFIG_FILE="tusb_config.h" CFG_TUSB_MCU=OPT_MCU_RP2040 ${port_defs})
        if(order STREQUAL "first_tusb")
            target_compile_definitions(${name} PRIVATE PROBE_TUSB_FIRST)
        endif()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "^${itfs} 64\n")
    endforeach()
endforeach()

# A build that leaves one of them out must stop, with the message that names it: the compiler is run
# on the probe with the others given.
foreach(missing MIMUSB_MAX_ENDPOINT_NUMBER MIMUSB_MAX_ENDPOINT_BYTES
                CFG_TUSB_OS CFG_TUSB_RHPORT0_MODE CFG_TUD_MAX_SPEED)
    mimusb_port_defs(7 port_defs)
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
            ${CMAKE_CURRENT_SOURCE_DIR}/port_probe.c)
    set_tests_properties(missing_${missing} PROPERTIES
        PASS_REGULAR_EXPRESSION "${missing} must be defined|MIMUSB_[A-Z_]+ must be defined")
endforeach()

# The limits of the RAM have no default: config.h stops the build of a port that has no
# mimusb_limits.h, or one that leaves a limit out, with the message that names it.
set(limit_names MIMUSB_MAX_INTERFACES MIMUSB_MAX_CONFIG_BYTES MIMUSB_MAX_BOS_BYTES MIMUSB_MAX_STRINGS
    MIMUSB_MAX_STRING_BYTES MIMUSB_MAX_REPORT_BYTES MIMUSB_CONTROL_DATA_MAX MIMUSB_EP_POOL_SLOTS
    MIMUSB_CONTROL_RULES MIMUSB_ANSWER_BYTES MIMUSB_LAST_SENT_SLOTS
    MIMUSB_TX_RING_BYTES MIMUSB_TX_RX_RING_BYTES)
mimusb_port_defs(7 port_defs)
set(limits_flags "")
foreach(def IN LISTS port_defs)
    list(APPEND limits_flags "-D${def}")
endforeach()
set(limits_compile ${CMAKE_C_COMPILER} -fsyntax-only ${limits_flags}
    -DCFG_TUSB_CONFIG_FILE="tusb_config.h" -DCFG_TUSB_MCU=OPT_MCU_RP2040
    -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/../common
    -I${CMAKE_CURRENT_SOURCE_DIR}/../../proto -I${TINYUSB_PATH}/src)
add_test(NAME missing_mimusb_limits_h
    COMMAND ${limits_compile} ${CMAKE_CURRENT_SOURCE_DIR}/limits_probe.c)
set_tests_properties(missing_mimusb_limits_h PROPERTIES
    PASS_REGULAR_EXPRESSION "mimusb_limits.h must be provided by the port")
file(READ ${limits_15}/mimusb_limits.h rp_limits)
foreach(missing IN LISTS limit_names)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/limits_without_${missing})
    string(REGEX REPLACE "#define ${missing} [^\n]*\n" "" without "${rp_limits}")
    file(WRITE ${dir}/mimusb_limits.h "${without}")
    add_test(NAME missing_${missing}_limit
        COMMAND ${limits_compile} -I${dir} ${CMAKE_CURRENT_SOURCE_DIR}/limits_probe.c)
    set_tests_properties(missing_${missing}_limit PROPERTIES
        PASS_REGULAR_EXPRESSION "${missing} must be defined by the port")
endforeach()

# The frame queue alone, against a fake UART that stays busy for a number of polls after each start.
foreach(itfs 15 7 6 3)
    mimusb_port_defs(${itfs} port_defs)
    add_executable(test_tx_${itfs} test_tx.c ../common/tx.c)
    target_include_directories(test_tx_${itfs} PRIVATE
        ${limits_${itfs}}
        ../common
        ../../proto
    )
    target_compile_definitions(test_tx_${itfs} PRIVATE ${port_defs})
    target_compile_options(test_tx_${itfs} PRIVATE -Wall -Wextra -UNDEBUG)
    add_test(NAME firmware_tx_${itfs} COMMAND test_tx_${itfs})
endforeach()
