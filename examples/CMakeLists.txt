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

add_executable(multitouch_gesture cpp/multitouch_gesture.cpp)
target_link_libraries(multitouch_gesture PRIVATE mimhid::mimhid)
