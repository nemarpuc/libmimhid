# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
#
# The hardware test programs (see README.md). A project of its own, never part of the library's
# build or of CI: they need a board, and Linux as the target.
#   cmake -S tests/hw -B <build dir> && cmake --build <build dir>
cmake_minimum_required(VERSION 3.20)
project(mimusb_hw_tests LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The library of this checkout, so the programs test what is in the tree.
set(MIMUSB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MIMUSB_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. mimusb)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

foreach(name hw_bulk hw_iso hw_cdc hw_keyboard_latency hw_control_rules hw_composite hw_bytes)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../examples/c)
    target_link_libraries(${name} PRIVATE mimusb::static PkgConfig::LIBUSB Threads::Threads)
endforeach()
