# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.20)
project(mimhid_consumer LANGUAGES C)

# Build against an installed package: -DMIMHID_COMPONENT=shared|static. A release archive holds
# both, so the release workflow configures this once per component against the same install.
set(MIMHID_COMPONENT "shared" CACHE STRING "Package component to link")
# The version of the release under test; the package is compatible within the same minor version.
set(MIMHID_VERSION "" CACHE STRING "Version to request from the package")
if(NOT MIMHID_VERSION)
    message(FATAL_ERROR "Pass -DMIMHID_VERSION=<major.minor.patch>")
endif()
if(MIMHID_COMPONENT STREQUAL "static")
    set(_consumer_target mimhid::mimhid_static)
    set(_consumer_type STATIC_LIBRARY)
elseif(MIMHID_COMPONENT STREQUAL "shared")
    set(_consumer_target mimhid::mimhid)
    set(_consumer_type SHARED_LIBRARY)
else()
    message(FATAL_ERROR "MIMHID_COMPONENT must be shared or static, not '${MIMHID_COMPONENT}'")
endif()
find_package(mimhid ${MIMHID_VERSION} REQUIRED COMPONENTS ${MIMHID_COMPONENT})
if(NOT mimhid_VERSION VERSION_EQUAL MIMHID_VERSION)
    message(FATAL_ERROR "found libmimhid ${mimhid_VERSION}, expected ${MIMHID_VERSION}")
endif()
# The target must be the library kind that was asked for, not the other one from the same prefix.
get_target_property(_consumer_actual ${_consumer_target} TYPE)
if(NOT _consumer_actual STREQUAL _consumer_type)
    message(FATAL_ERROR "${_consumer_target} is a ${_consumer_actual}, expected ${_consumer_type}")
endif()
add_executable(consumer main.c)
target_compile_definitions(consumer PRIVATE "MIMHID_CONSUMER_COMPONENT=\"${MIMHID_COMPONENT}\"")
target_link_libraries(consumer PRIVATE ${_consumer_target})
