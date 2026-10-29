# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
# Test executables. Included from the top-level CMakeLists.txt when MIMHID_BUILD_TESTS is on.

function(mimhid_test_flags target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
    if(MSVC)
        # The tests rely on assert() and checks that must stay active in Release builds.
        target_compile_options(${target} PRIVATE /W4 /EHsc /GR- /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fno-rtti -UNDEBUG)
    endif()
endfunction()

# Unit tests of the HID layer and the profile state machines: they link the internal objects.
add_executable(mimhid_tests
    tests/unit/test_main.cpp
    tests/unit/test_item_writer.cpp
    tests/unit/test_link.cpp
    tests/unit/test_profiles.cpp
    $<TARGET_OBJECTS:mimhid_test_objects>)
target_include_directories(mimhid_tests PRIVATE include src proto tests)
target_link_libraries(mimhid_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(mimhid_tests PRIVATE setupapi advapi32)
endif()
mimhid_test_flags(mimhid_tests)
add_test(NAME mimhid.unit COMMAND mimhid_tests)

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

# The public headers must be usable from C and from C++.
add_executable(mimhid_c_abi_test tests/abi/test_c_abi.c)
target_link_libraries(mimhid_c_abi_test PRIVATE mimhid::mimhid)
if(NOT MSVC)
    target_compile_options(mimhid_c_abi_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME mimhid.c-abi COMMAND mimhid_c_abi_test)

add_executable(mimhid_cpp_wrapper_test tests/abi/test_cpp_wrapper.cpp)
target_link_libraries(mimhid_cpp_wrapper_test PRIVATE mimhid::mimhid)
mimhid_test_flags(mimhid_cpp_wrapper_test)
add_test(NAME mimhid.cpp-wrapper COMMAND mimhid_cpp_wrapper_test)

# End to end: the host library against the real firmware core running on the build host.
# Configure with -DMIMHID_TINYUSB_PATH=<tinyusb checkout> to enable (POSIX only).
set(MIMHID_TINYUSB_PATH "" CACHE PATH "TinyUSB checkout for the end-to-end tests")
if(MIMHID_TINYUSB_PATH AND NOT WIN32)
    foreach(itfs 7 3)
        add_executable(mimhid_e2e_${itfs}
            tests/e2e/test_e2e.cpp
            tests/e2e/fake_board.c
            firmware/common/core.c
            firmware/common/control.c
            firmware/common/ep0.c
            firmware/common/usb_descriptors.c
            $<TARGET_OBJECTS:mimhid_test_objects>)
        target_include_directories(mimhid_e2e_${itfs} PRIVATE
            include src proto tests tests/e2e
            firmware/tests firmware/common ${MIMHID_TINYUSB_PATH}/src)
        target_compile_definitions(mimhid_e2e_${itfs} PRIVATE
            MIMHID_HID_ITFS=${itfs} E2E_HID_ITFS=${itfs}
            CFG_TUSB_CONFIG_FILE="tusb_config.h")
        target_link_libraries(mimhid_e2e_${itfs} PRIVATE Threads::Threads)
        add_test(NAME mimhid.e2e-${itfs} COMMAND mimhid_e2e_${itfs})
    endforeach()
endif()
