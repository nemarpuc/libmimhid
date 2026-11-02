# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
if(NOT DEFINED MIMHID_SOURCE_DIR OR NOT DEFINED MIMHID_TEST_BINARY_DIR OR
   NOT DEFINED MIMHID_GENERATOR OR NOT DEFINED MIMHID_PROJECT_VERSION OR
   NOT DEFINED MIMHID_PROJECT_HOMEPAGE_URL)
  message(FATAL_ERROR "package component contract test inputs are incomplete")
endif()

# This is a configure-package contract test. It deliberately does not start a
# nested C/C++ toolchain: doing so made the test depend on the host/target
# pairing of the Visual Studio runner even though no object file is required to
# validate CMake component selection.
include(CMakePackageConfigHelpers)

set(_MIMHID_STAGE "${MIMHID_TEST_BINARY_DIR}/stage")
set(_MIMHID_PACKAGE_DIR "${_MIMHID_STAGE}/lib/cmake/mimhid")
set(_MIMHID_CONSUMER_SOURCE "${MIMHID_TEST_BINARY_DIR}/consumer")
file(REMOVE_RECURSE "${MIMHID_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${_MIMHID_PACKAGE_DIR}" "${_MIMHID_CONSUMER_SOURCE}")

# Generate the exact static-only installed configuration directly from the
# production template, with the substitution values of a static-only build; no compiler
# discovery is needed.
set(PROJECT_VERSION "${MIMHID_PROJECT_VERSION}")
set(PROJECT_HOMEPAGE_URL "${MIMHID_PROJECT_HOMEPAGE_URL}")
set(MIMHID_BUILD_SHARED OFF)
set(MIMHID_BUILD_STATIC ON)
configure_package_config_file(
    "${MIMHID_SOURCE_DIR}/cmake/mimhid-config.cmake.in"
    "${_MIMHID_PACKAGE_DIR}/mimhid-config.cmake"
    INSTALL_DESTINATION "lib/cmake/mimhid")
write_basic_package_version_file(
    "${_MIMHID_PACKAGE_DIR}/mimhid-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

# Deliberately place both exports in the stage. The embedded OFF/ON build
# contract must still reject shared and accept static.
file(WRITE "${MIMHID_TEST_BINARY_DIR}/dummy-static.lib" "")
file(WRITE "${_MIMHID_PACKAGE_DIR}/mimhid-shared-targets.cmake"
    "add_library(mimhid::mimhid SHARED IMPORTED)\n")
file(WRITE "${_MIMHID_PACKAGE_DIR}/mimhid-static-targets.cmake"
    "add_library(mimhid::mimhid_static STATIC IMPORTED)\n"
    "set_target_properties(mimhid::mimhid_static PROPERTIES "
    "IMPORTED_LOCATION \"${MIMHID_TEST_BINARY_DIR}/dummy-static.lib\")\n")

# LANGUAGES NONE is intentional. Component selection is CMake package metadata
# behavior, so the test must behave identically on x64 and ARM64.
file(WRITE "${_MIMHID_CONSUMER_SOURCE}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(mimhid_component_contract LANGUAGES NONE)
if(NOT DEFINED MIMHID_FIND_COMPONENTS)
  message(FATAL_ERROR "MIMHID_FIND_COMPONENTS is required")
endif()
find_package(mimhid CONFIG REQUIRED COMPONENTS ${MIMHID_FIND_COMPONENTS})
if(DEFINED MIMHID_EXPECT_TARGET AND NOT TARGET "${MIMHID_EXPECT_TARGET}")
  message(FATAL_ERROR "Expected installed target does not exist: ${MIMHID_EXPECT_TARGET}")
endif()
]=])

set(_MIMHID_CONSUMER_BASE
    "${CMAKE_COMMAND}"
    -S "${_MIMHID_CONSUMER_SOURCE}"
    -G "${MIMHID_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${_MIMHID_STAGE}")

execute_process(
    COMMAND ${_MIMHID_CONSUMER_BASE}
        -B "${MIMHID_TEST_BINARY_DIR}/wrong-shared"
        -DMIMHID_FIND_COMPONENTS=shared
    RESULT_VARIABLE _MIMHID_WRONG_SHARED_RESULT
    OUTPUT_VARIABLE _MIMHID_WRONG_SHARED_STDOUT
    ERROR_VARIABLE _MIMHID_WRONG_SHARED_STDERR)
if(_MIMHID_WRONG_SHARED_RESULT EQUAL 0)
  message(FATAL_ERROR
      "static-only package accepted shared despite its embedded build contract\n"
      "${_MIMHID_WRONG_SHARED_STDOUT}\n${_MIMHID_WRONG_SHARED_STDERR}")
endif()

execute_process(
    COMMAND ${_MIMHID_CONSUMER_BASE}
        -B "${MIMHID_TEST_BINARY_DIR}/static"
        -DMIMHID_FIND_COMPONENTS=static
        -DMIMHID_EXPECT_TARGET=mimhid::mimhid_static
    RESULT_VARIABLE _MIMHID_STATIC_RESULT
    OUTPUT_VARIABLE _MIMHID_STATIC_STDOUT
    ERROR_VARIABLE _MIMHID_STATIC_STDERR)
if(NOT _MIMHID_STATIC_RESULT EQUAL 0)
  message(FATAL_ERROR
      "static-only package rejected its static component (${_MIMHID_STATIC_RESULT})\n"
      "${_MIMHID_STATIC_STDOUT}\n${_MIMHID_STATIC_STDERR}")
endif()
