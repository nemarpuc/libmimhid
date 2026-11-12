# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
if(NOT DEFINED MIMUSB_SOURCE_DIR OR NOT DEFINED MIMUSB_TEST_BINARY_DIR OR
   NOT DEFINED MIMUSB_GENERATOR OR NOT DEFINED MIMUSB_PROJECT_VERSION OR
   NOT DEFINED MIMUSB_PROJECT_HOMEPAGE_URL)
  message(FATAL_ERROR "package component contract test inputs are incomplete")
endif()

# This is a configure-package contract test. It deliberately does not start a
# nested C/C++ toolchain: doing so made the test depend on the host/target
# pairing of the Visual Studio runner even though no object file is required to
# validate CMake component selection.
include(CMakePackageConfigHelpers)

set(_MIMUSB_STAGE "${MIMUSB_TEST_BINARY_DIR}/stage")
set(_MIMUSB_PACKAGE_DIR "${_MIMUSB_STAGE}/lib/cmake/mimusb")
set(_MIMUSB_CONSUMER_SOURCE "${MIMUSB_TEST_BINARY_DIR}/consumer")
file(REMOVE_RECURSE "${MIMUSB_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${_MIMUSB_PACKAGE_DIR}" "${_MIMUSB_CONSUMER_SOURCE}")

# Generate the exact static-only installed configuration directly from the
# production template, with the substitution values of a static-only build; no compiler
# discovery is needed.
set(PROJECT_VERSION "${MIMUSB_PROJECT_VERSION}")
set(PROJECT_HOMEPAGE_URL "${MIMUSB_PROJECT_HOMEPAGE_URL}")
set(MIMUSB_BUILD_SHARED OFF)
set(MIMUSB_BUILD_STATIC ON)
configure_package_config_file(
    "${MIMUSB_SOURCE_DIR}/cmake/mimusb-config.cmake.in"
    "${_MIMUSB_PACKAGE_DIR}/mimusb-config.cmake"
    INSTALL_DESTINATION "lib/cmake/mimusb")
write_basic_package_version_file(
    "${_MIMUSB_PACKAGE_DIR}/mimusb-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

# Deliberately place both exports in the stage. The embedded OFF/ON build
# contract must still reject shared and accept static.
file(WRITE "${MIMUSB_TEST_BINARY_DIR}/dummy-static.lib" "")
file(WRITE "${_MIMUSB_PACKAGE_DIR}/mimusb-shared-targets.cmake"
    "add_library(mimusb::mimusb SHARED IMPORTED)\n")
file(WRITE "${_MIMUSB_PACKAGE_DIR}/mimusb-static-targets.cmake"
    "add_library(mimusb::mimusb_static STATIC IMPORTED)\n"
    "set_target_properties(mimusb::mimusb_static PROPERTIES "
    "IMPORTED_LOCATION \"${MIMUSB_TEST_BINARY_DIR}/dummy-static.lib\")\n")

# LANGUAGES NONE is intentional. Component selection is CMake package metadata
# behavior, so the test must behave identically on x64 and ARM64.
file(WRITE "${_MIMUSB_CONSUMER_SOURCE}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(mimusb_component_contract LANGUAGES NONE)
if(NOT DEFINED MIMUSB_FIND_COMPONENTS)
  message(FATAL_ERROR "MIMUSB_FIND_COMPONENTS is required")
endif()
find_package(mimusb CONFIG REQUIRED COMPONENTS ${MIMUSB_FIND_COMPONENTS})
if(DEFINED MIMUSB_EXPECT_TARGET AND NOT TARGET "${MIMUSB_EXPECT_TARGET}")
  message(FATAL_ERROR "Expected installed target does not exist: ${MIMUSB_EXPECT_TARGET}")
endif()
]=])

set(_MIMUSB_CONSUMER_BASE
    "${CMAKE_COMMAND}"
    -S "${_MIMUSB_CONSUMER_SOURCE}"
    -G "${MIMUSB_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${_MIMUSB_STAGE}")

execute_process(
    COMMAND ${_MIMUSB_CONSUMER_BASE}
        -B "${MIMUSB_TEST_BINARY_DIR}/wrong-shared"
        -DMIMUSB_FIND_COMPONENTS=shared
    RESULT_VARIABLE _MIMUSB_WRONG_SHARED_RESULT
    OUTPUT_VARIABLE _MIMUSB_WRONG_SHARED_STDOUT
    ERROR_VARIABLE _MIMUSB_WRONG_SHARED_STDERR)
if(_MIMUSB_WRONG_SHARED_RESULT EQUAL 0)
  message(FATAL_ERROR
      "static-only package accepted shared despite its embedded build contract\n"
      "${_MIMUSB_WRONG_SHARED_STDOUT}\n${_MIMUSB_WRONG_SHARED_STDERR}")
endif()

execute_process(
    COMMAND ${_MIMUSB_CONSUMER_BASE}
        -B "${MIMUSB_TEST_BINARY_DIR}/static"
        -DMIMUSB_FIND_COMPONENTS=static
        -DMIMUSB_EXPECT_TARGET=mimusb::mimusb_static
    RESULT_VARIABLE _MIMUSB_STATIC_RESULT
    OUTPUT_VARIABLE _MIMUSB_STATIC_STDOUT
    ERROR_VARIABLE _MIMUSB_STATIC_STDERR)
if(NOT _MIMUSB_STATIC_RESULT EQUAL 0)
  message(FATAL_ERROR
      "static-only package rejected its static component (${_MIMUSB_STATIC_RESULT})\n"
      "${_MIMUSB_STATIC_STDOUT}\n${_MIMUSB_STATIC_STDERR}")
endif()
