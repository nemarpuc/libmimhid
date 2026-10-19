# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.20)
project(mimhid_consumer LANGUAGES C)

# Build against an installed package: -DMIMHID_COMPONENT=shared|static
set(MIMHID_COMPONENT "shared" CACHE STRING "Package component to link")
# The version of the release under test; the package is compatible within the same minor version.
set(MIMHID_VERSION "" CACHE STRING "Version to request from the package")
if(NOT MIMHID_VERSION)
    message(FATAL_ERROR "Pass -DMIMHID_VERSION=<major.minor.patch>")
endif()
find_package(mimhid ${MIMHID_VERSION} REQUIRED COMPONENTS ${MIMHID_COMPONENT})
add_executable(consumer main.c)
if(MIMHID_COMPONENT STREQUAL "static")
    target_link_libraries(consumer PRIVATE mimhid::mimhid_static)
else()
    target_link_libraries(consumer PRIVATE mimhid::mimhid)
endif()
