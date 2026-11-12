# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
if(NOT DEFINED MIMUSB_SOURCE_DIR OR NOT DEFINED MIMUSB_TEST_BINARY_DIR OR
   NOT DEFINED MIMUSB_GENERATOR)
  message(FATAL_ERROR "package layout test inputs are incomplete")
endif()

set(_MIMUSB_CONFIGURE_COMMAND
    "${CMAKE_COMMAND}"
    -S "${MIMUSB_SOURCE_DIR}"
    -B "${MIMUSB_TEST_BINARY_DIR}"
    -G "${MIMUSB_GENERATOR}"
    -DMIMUSB_BUILD_SHARED=ON
    -DMIMUSB_BUILD_STATIC=ON
    -DMIMUSB_BUILD_TESTS=OFF
    -DMIMUSB_BUILD_EXAMPLES=OFF
    -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu)
if(DEFINED MIMUSB_GENERATOR_PLATFORM AND
   NOT MIMUSB_GENERATOR_PLATFORM STREQUAL "")
  list(APPEND _MIMUSB_CONFIGURE_COMMAND -A "${MIMUSB_GENERATOR_PLATFORM}")
endif()
if(DEFINED MIMUSB_GENERATOR_TOOLSET AND
   NOT MIMUSB_GENERATOR_TOOLSET STREQUAL "")
  list(APPEND _MIMUSB_CONFIGURE_COMMAND -T "${MIMUSB_GENERATOR_TOOLSET}")
endif()

execute_process(
    COMMAND ${_MIMUSB_CONFIGURE_COMMAND}
    RESULT_VARIABLE _MIMUSB_CONFIGURE_RESULT
    OUTPUT_VARIABLE _MIMUSB_CONFIGURE_STDOUT
    ERROR_VARIABLE _MIMUSB_CONFIGURE_STDERR)
if(NOT _MIMUSB_CONFIGURE_RESULT EQUAL 0)
  message(FATAL_ERROR
      "nested multiarch configure failed (${_MIMUSB_CONFIGURE_RESULT})\n"
      "${_MIMUSB_CONFIGURE_STDOUT}\n${_MIMUSB_CONFIGURE_STDERR}")
endif()

function(mimusb_require_contains contents expected label)
  string(FIND "${contents}" "${expected}" _MIMUSB_MATCH_INDEX)
  if(_MIMUSB_MATCH_INDEX EQUAL -1)
    message(FATAL_ERROR "${label} does not contain expected text: ${expected}")
  endif()
endfunction()

file(READ "${MIMUSB_TEST_BINARY_DIR}/mimusb.pc" _MIMUSB_PC)
mimusb_require_contains(
    "${_MIMUSB_PC}" "prefix=\${pcfiledir}/../../.." "multiarch mimusb.pc")
mimusb_require_contains(
    "${_MIMUSB_PC}"
    "libdir=\${exec_prefix}/lib/x86_64-linux-gnu"
    "multiarch mimusb.pc")
mimusb_require_contains(
    "${_MIMUSB_PC}" "includedir=\${prefix}/include" "multiarch mimusb.pc")

file(READ "${MIMUSB_TEST_BINARY_DIR}/mimusb-config.cmake" _MIMUSB_CONFIG)
mimusb_require_contains(
    "${_MIMUSB_CONFIG}"
    [=[get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../" ABSOLUTE)]=]
    "multiarch CMake package config")
# libmimusb has no external dependency, so the package config needs no library hint: the only
# multiarch-sensitive part is how deep PACKAGE_INIT climbs to the prefix.
