# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(mimhid_examples LANGUAGES C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 20)
    find_package(mimhid REQUIRED)
endif()

add_executable(basic_keyboard_mouse c/basic_keyboard_mouse.c)
target_link_libraries(basic_keyboard_mouse PRIVATE mimhid::mimhid)

add_executable(control_transfers c/control_transfers.c)
target_link_libraries(control_transfers PRIVATE mimhid::mimhid)

add_executable(vendor_bulk c/vendor_bulk.c)
target_link_libraries(vendor_bulk PRIVATE mimhid::mimhid)

add_executable(hid_interface c/hid_interface.c)
target_link_libraries(hid_interface PRIVATE mimhid::mimhid)

add_executable(multitouch_gesture cpp/multitouch_gesture.cpp)
target_link_libraries(multitouch_gesture PRIVATE mimhid::mimhid)

# One example per mimhid_profile_kind value. Each builds a complete Spec and
# needs no board, so every one of them is runnable in CI.
set(MIMHID_PROFILE_EXAMPLES
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
foreach(mimhid_profile_example IN LISTS MIMHID_PROFILE_EXAMPLES)
    add_executable(mimhid_example_${mimhid_profile_example}
        c/profiles/${mimhid_profile_example}.c)
    target_link_libraries(mimhid_example_${mimhid_profile_example} PRIVATE mimhid::mimhid)
    set_target_properties(mimhid_example_${mimhid_profile_example} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    if(MIMHID_BUILD_TESTS)
        string(REPLACE "_" "-" mimhid_profile_test_name "${mimhid_profile_example}")
        add_test(NAME mimhid.example-profile-${mimhid_profile_test_name}
            COMMAND mimhid_example_${mimhid_profile_example})
    endif()
endforeach()

# Programs to run with a real board and a real target machine, and to watch
# (docs/QUICKSTART.md). Unlike the examples above they are never registered as
# ctest cases.
set(MIMHID_VERIFY_EXAMPLES keyboard touch mouse toggle all)
foreach(mimhid_verify_example IN LISTS MIMHID_VERIFY_EXAMPLES)
    add_executable(mimhid_verify_${mimhid_verify_example}
        c/verify/verify_${mimhid_verify_example}.c)
    target_link_libraries(mimhid_verify_${mimhid_verify_example} PRIVATE mimhid::mimhid)
    target_include_directories(mimhid_verify_${mimhid_verify_example} PRIVATE c/verify)
    if(NOT WIN32)
        target_link_libraries(mimhid_verify_${mimhid_verify_example} PRIVATE m)
    endif()
    set_target_properties(mimhid_verify_${mimhid_verify_example} PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
endforeach()
