# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.20)
project(mimhid_consumer LANGUAGES C)

# Build against an installed package: -DMIMHID_COMPONENT=shared|static
set(MIMHID_COMPONENT "shared" CACHE STRING "Package component to link")
find_package(mimhid 1.0 REQUIRED COMPONENTS ${MIMHID_COMPONENT})
add_executable(consumer main.c)
if(MIMHID_COMPONENT STREQUAL "static")
    target_link_libraries(consumer PRIVATE mimhid::mimhid_static)
else()
    target_link_libraries(consumer PRIVATE mimhid::mimhid)
endif()
