# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(mimusb_examples LANGUAGES C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 20)
    find_package(mimusb REQUIRED)
endif()

add_executable(basic_keyboard_mouse c/basic_keyboard_mouse.c)
target_link_libraries(basic_keyboard_mouse PRIVATE mimusb::mimusb)

add_executable(control_transfers c/control_transfers.c)
target_link_libraries(control_transfers PRIVATE mimusb::mimusb)

add_executable(vendor_bulk c/vendor_bulk.c)
target_link_libraries(vendor_bulk PRIVATE mimusb::mimusb)

add_executable(device_from_descriptors c/device_from_descriptors.c)
target_link_libraries(device_from_descriptors PRIVATE mimusb::mimusb)

add_executable(winusb_bulk c/winusb_bulk.c)
target_link_libraries(winusb_bulk PRIVATE mimusb::mimusb)

add_executable(hid_interface c/hid_interface.c)
target_link_libraries(hid_interface PRIVATE mimusb::mimusb)

add_executable(multitouch_gesture cpp/multitouch_gesture.cpp)
target_link_libraries(multitouch_gesture PRIVATE mimusb::mimusb)

# One example per mimusb_profile_kind value. Each builds a complete Spec and
# needs no board, so every one of them is runnable in CI.
set(MIMUSB_PROFILE_EXAMPLES
    keyboard
    mouse
    toggle
    gamepad
    touchscreen
    touchpad
    pen
    digitizer
    keyboard_battery
    raw)
foreach(mimusb_profile_example IN LISTS MIMUSB_PROFILE_EXAMPLES)
    add_executable(mimusb_example_${mimusb_profile_example}
        c/profiles/${mimusb_profile_example}.c)
    target_link_libraries(mimusb_example_${mimusb_profile_example} PRIVATE mimusb::mimusb)
    set_target_properties(mimusb_example_${mimusb_profile_example} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    if(MIMUSB_BUILD_TESTS)
        string(REPLACE "_" "-" mimusb_profile_test_name "${mimusb_profile_example}")
        add_test(NAME mimusb.example-profile-${mimusb_profile_test_name}
            COMMAND mimusb_example_${mimusb_profile_example})
    endif()
endforeach()

# Programs to run with a real board and a real target machine, and to watch
# (docs/QUICKSTART.md). Unlike the examples above they are never registered as
# ctest cases.
set(MIMUSB_VERIFY_EXAMPLES keyboard touch mouse toggle all)
foreach(mimusb_verify_example IN LISTS MIMUSB_VERIFY_EXAMPLES)
    add_executable(mimusb_verify_${mimusb_verify_example}
        c/verify/verify_${mimusb_verify_example}.c)
    target_link_libraries(mimusb_verify_${mimusb_verify_example} PRIVATE mimusb::mimusb)
    target_include_directories(mimusb_verify_${mimusb_verify_example} PRIVATE c/verify)
    if(NOT WIN32)
        target_link_libraries(mimusb_verify_${mimusb_verify_example} PRIVATE m)
    endif()
    set_target_properties(mimusb_verify_${mimusb_verify_example} PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
endforeach()
