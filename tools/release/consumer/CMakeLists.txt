# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.20)
project(mimusb_consumer LANGUAGES C)

# Build against an installed package: -DMIMUSB_COMPONENT=shared|static. A release archive holds
# both, so the release workflow configures this once per component against the same install.
set(MIMUSB_COMPONENT "shared" CACHE STRING "Package component to link")
# The version of the release under test; the package is compatible within the same minor version.
set(MIMUSB_VERSION "" CACHE STRING "Version to request from the package")
if(NOT MIMUSB_VERSION)
    message(FATAL_ERROR "Pass -DMIMUSB_VERSION=<major.minor.patch>")
endif()
if(MIMUSB_COMPONENT STREQUAL "static")
    set(_consumer_target mimusb::mimusb_static)
    set(_consumer_type STATIC_LIBRARY)
elseif(MIMUSB_COMPONENT STREQUAL "shared")
    set(_consumer_target mimusb::mimusb)
    set(_consumer_type SHARED_LIBRARY)
else()
    message(FATAL_ERROR "MIMUSB_COMPONENT must be shared or static, not '${MIMUSB_COMPONENT}'")
endif()
find_package(mimusb ${MIMUSB_VERSION} REQUIRED COMPONENTS ${MIMUSB_COMPONENT})
if(NOT mimusb_VERSION VERSION_EQUAL MIMUSB_VERSION)
    message(FATAL_ERROR "found libmimusb ${mimusb_VERSION}, expected ${MIMUSB_VERSION}")
endif()
# The target must be the library kind that was asked for, not the other one from the same prefix.
get_target_property(_consumer_actual ${_consumer_target} TYPE)
if(NOT _consumer_actual STREQUAL _consumer_type)
    message(FATAL_ERROR "${_consumer_target} is a ${_consumer_actual}, expected ${_consumer_type}")
endif()
add_executable(consumer main.c)
target_compile_definitions(consumer PRIVATE "MIMUSB_CONSUMER_COMPONENT=\"${MIMUSB_COMPONENT}\"")
target_link_libraries(consumer PRIVATE ${_consumer_target})
