# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
# Test executables. Included from the top-level CMakeLists.txt when MIMHID_BUILD_TESTS is on.

function(mimhid_test_flags target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
    if(MSVC)
        # The tests rely on assert() and checks that must stay active in Release builds.
        target_compile_options(${target} PRIVATE /W4 /EHsc /GR- /UNDEBUG)
        if(MIMHID_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fno-rtti -UNDEBUG)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
            # GCC 12+ reports these two from inlined std::vector::insert/uninitialized_copy of
            # small initializer lists in -O2 builds (GCC bug 105329 and related). They are false
            # positives in libstdc++, they only fire in test helpers, and the library itself keeps
            # the full warning set.
            target_compile_options(${target} PRIVATE -Wno-array-bounds -Wno-stringop-overflow)
        endif()
        if(MIMHID_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Unit tests of the HID layer and the profile state machines: they link the internal objects.
add_executable(mimhid_tests
    tests/unit/test_main.cpp
    tests/unit/test_item_writer.cpp
    tests/unit/test_descriptor_builder.cpp
    tests/unit/test_validator_parser_state.cpp
    tests/unit/test_validator_arithmetic.cpp
    tests/unit/test_validator_encoding.cpp
    tests/unit/test_frame.cpp
    tests/unit/test_descriptor_composer.cpp
    tests/unit/test_descriptor_walker.cpp
    tests/unit/test_profiles_common.cpp
    tests/unit/test_profiles_keyboard.cpp
    tests/unit/test_profiles_mouse.cpp
    tests/unit/test_profiles_usage_controls.cpp
    tests/unit/test_profiles_gamepad.cpp
    tests/unit/test_profiles_touch.cpp
    tests/unit/test_profiles_pen.cpp
    tests/unit/test_profiles_raw.cpp
    tests/unit/test_profiles_battery.cpp
    tests/unit/test_error_detail.cpp
    tests/unit/test_api_arguments.cpp
    tests/unit/test_spec_rejections.cpp
    tests/unit/test_input_rejections.cpp
    $<TARGET_OBJECTS:mimhid_test_objects>)
target_include_directories(mimhid_tests PRIVATE include src proto tests)
target_link_libraries(mimhid_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(mimhid_tests PRIVATE setupapi advapi32)
endif()
mimhid_test_flags(mimhid_tests)
add_test(NAME mimhid.unit COMMAND mimhid_tests)

# Scripted board: the transport and the C ABI against a board the test plays on a pseudo-terminal,
# which can NAK, stay silent, stop reading or hang up (POSIX only; no TinyUSB needed).
if(NOT WIN32)
    # The rate of the library's end of a pseudo-terminal, which the scripted board compares with
    # its own; C, so outside the C++ flags of mimhid_test_flags.
    add_library(mimhid_scripted_pty OBJECT tests/sim/pty_baud.c)
    add_executable(mimhid_scripted_tests
        tests/scripted/test_scripted_main.cpp
        tests/scripted/scripted_board.cpp
        tests/scripted/test_api_commands.cpp
        tests/scripted/test_api_events.cpp
        tests/scripted/test_api_open.cpp
        tests/scripted/test_api_raw_node.cpp
        tests/scripted/test_api_generic.cpp
        tests/scripted/test_api_reports.cpp
        tests/scripted/test_handshaker.cpp
        tests/scripted/test_link.cpp
        tests/scripted/test_ports_linux.cpp
        tests/scripted/test_serial_port.cpp
        $<TARGET_OBJECTS:mimhid_scripted_pty>
        $<TARGET_OBJECTS:mimhid_test_objects>)
    target_include_directories(mimhid_scripted_tests PRIVATE include src proto tests tests/scripted)
    target_link_libraries(mimhid_scripted_tests PRIVATE Threads::Threads)
    mimhid_test_flags(mimhid_scripted_tests)
    add_test(NAME mimhid.scripted COMMAND mimhid_scripted_tests)
endif()

# Descriptors for every profile must match the golden files byte for byte.
add_executable(mimhid_export_descriptors tests/hidtools/export_descriptors.cpp)
target_link_libraries(mimhid_export_descriptors PRIVATE mimhid::mimhid)
target_include_directories(mimhid_export_descriptors PRIVATE src)
mimhid_test_flags(mimhid_export_descriptors)
add_test(NAME mimhid.golden-descriptors
    COMMAND mimhid_export_descriptors
        ${CMAKE_CURRENT_BINARY_DIR}/golden-descriptor-output
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden
        ${CMAKE_CURRENT_BINARY_DIR}/support-manifests.tsv)

# The installed package: a multiarch libdir must reach the .pc and the package config, and the
# component selection of find_package(mimhid COMPONENTS shared|static) must follow what was built.
add_test(NAME mimhid.package-multiarch-layout
    COMMAND ${CMAKE_COMMAND}
        -DMIMHID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DMIMHID_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-multiarch-layout
        -DMIMHID_GENERATOR=${CMAKE_GENERATOR}
        -DMIMHID_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DMIMHID_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/package_multiarch_layout.cmake)
add_test(NAME mimhid.package-component-contract
    COMMAND ${CMAKE_COMMAND}
        -DMIMHID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DMIMHID_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-component-contract
        -DMIMHID_GENERATOR=${CMAKE_GENERATOR}
        -DMIMHID_PROJECT_VERSION=${PROJECT_VERSION}
        -DMIMHID_PROJECT_HOMEPAGE_URL=${PROJECT_HOMEPAGE_URL}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/package_component_contract.cmake)

# Every size, offset and constant of the C ABI must match the golden layout.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_executable(mimhid_abi_layout_oracle tests/abi/layout_oracle.c)
target_include_directories(mimhid_abi_layout_oracle PRIVATE include)
set_target_properties(mimhid_abi_layout_oracle PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(mimhid_abi_layout_oracle PRIVATE /W4 /utf-8)
    if(MIMHID_WERROR)
        target_compile_options(mimhid_abi_layout_oracle PRIVATE /WX)
    endif()
else()
    target_compile_options(mimhid_abi_layout_oracle PRIVATE -Wall -Wextra -Wpedantic -fshort-enums)
    if(MIMHID_WERROR)
        target_compile_options(mimhid_abi_layout_oracle PRIVATE -Werror)
    endif()
endif()
add_test(NAME mimhid.abi-layout-oracle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/check_abi_golden.py
        $<TARGET_FILE:mimhid_abi_layout_oracle>)

# The public headers must be usable from C and from C++.
add_executable(mimhid_c_abi_test tests/abi/test_c_abi.c)
target_link_libraries(mimhid_c_abi_test PRIVATE mimhid::mimhid)
if(MSVC)
    target_compile_options(mimhid_c_abi_test PRIVATE /W4 /utf-8)
    if(MIMHID_WERROR)
        target_compile_options(mimhid_c_abi_test PRIVATE /WX)
    endif()
else()
    target_compile_options(mimhid_c_abi_test PRIVATE -Wall -Wextra -Wpedantic)
    if(MIMHID_WERROR)
        target_compile_options(mimhid_c_abi_test PRIVATE -Werror)
    endif()
endif()
add_test(NAME mimhid.c-abi COMMAND mimhid_c_abi_test)

add_executable(mimhid_cpp_wrapper_test tests/abi/test_cpp_wrapper.cpp)
target_link_libraries(mimhid_cpp_wrapper_test PRIVATE mimhid::mimhid)
mimhid_test_flags(mimhid_cpp_wrapper_test)
add_test(NAME mimhid.cpp-wrapper COMMAND mimhid_cpp_wrapper_test)

# Simulated board: the host library against the real firmware core running on the build host.
# Configure with -DMIMHID_TINYUSB_PATH=<tinyusb checkout> to enable (POSIX only).
set(MIMHID_TINYUSB_PATH "" CACHE PATH "TinyUSB checkout for the simulated-board tests")
if(MIMHID_TINYUSB_PATH AND NOT WIN32)
    # One scenario file per area (tests/sim/test_sim_*.cpp), the shared harness, the simulated
    # board and the firmware core it runs.
    set(MIMHID_SIM_SOURCES
        tests/sim/test_sim_main.cpp
        tests/sim/sim_support.cpp
        tests/sim/test_sim_arguments.cpp
        tests/sim/test_sim_bytes.cpp
        tests/sim/test_sim_control.cpp
        tests/sim/test_sim_events.cpp
        tests/sim/test_sim_generic.cpp
        tests/sim/test_sim_high_speed.cpp
        tests/sim/test_sim_line_rate.cpp
        tests/sim/test_sim_open.cpp
        tests/sim/test_sim_reports.cpp
        tests/sim/fake_board.c
        tests/sim/pty_baud.c
        firmware/common/core.c
        firmware/common/control.c
        firmware/common/ep0.c
        firmware/common/usb_descriptors.c
        firmware/common/generic.c)
    foreach(itfs 15 7 3) # the highest endpoint number the simulated board has
        add_executable(mimhid_sim_${itfs}
            ${MIMHID_SIM_SOURCES}
            $<TARGET_OBJECTS:mimhid_test_objects>)
        target_include_directories(mimhid_sim_${itfs} PRIVATE
            include src proto tests tests/sim
            firmware/tests firmware/common ${MIMHID_TINYUSB_PATH}/src)
        # What a port's build states (firmware/common/core.h): the highest endpoint number, the
        # packet size and the speed.
        target_compile_definitions(mimhid_sim_${itfs} PRIVATE
            SIM_ENDPOINTS=${itfs}
            MIMHID_MAX_ENDPOINT_NUMBER=${itfs} MIMHID_MAX_ENDPOINT_BYTES=64
            CFG_TUSB_OS=OPT_OS_NONE
            "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
            CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
            CFG_TUSB_CONFIG_FILE="tusb_config.h")
        target_link_libraries(mimhid_sim_${itfs} PRIVATE Threads::Threads)
        add_test(NAME mimhid.sim-${itfs} COMMAND mimhid_sim_${itfs})
    endforeach()

    # The rules only a high-speed board has, against the firmware core built for high speed.
    add_executable(mimhid_sim_hs
        ${MIMHID_SIM_SOURCES}
        $<TARGET_OBJECTS:mimhid_test_objects>)
    target_include_directories(mimhid_sim_hs PRIVATE
        include src proto tests tests/sim
        firmware/tests firmware/common ${MIMHID_TINYUSB_PATH}/src)
    target_compile_definitions(mimhid_sim_hs PRIVATE
        SIM_ENDPOINTS=7 SIM_HIGH_SPEED
        MIMHID_MAX_ENDPOINT_NUMBER=7 MIMHID_MAX_ENDPOINT_BYTES=1024
        CFG_TUSB_OS=OPT_OS_NONE
        CFG_TUSB_CONFIG_FILE="tusb_config.h"
        CFG_TUD_MAX_SPEED=OPT_MODE_HIGH_SPEED
        "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_HIGH_SPEED)")
    target_link_libraries(mimhid_sim_hs PRIVATE Threads::Threads)
    add_test(NAME mimhid.sim-hs COMMAND mimhid_sim_hs)
endif()
