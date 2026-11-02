# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
if(NOT DEFINED MIMHID_SOURCE_DIR OR NOT DEFINED MIMHID_TEST_BINARY_DIR OR
   NOT DEFINED MIMHID_GENERATOR)
  message(FATAL_ERROR "package layout test inputs are incomplete")
endif()

set(_MIMHID_CONFIGURE_COMMAND
    "${CMAKE_COMMAND}"
    -S "${MIMHID_SOURCE_DIR}"
    -B "${MIMHID_TEST_BINARY_DIR}"
    -G "${MIMHID_GENERATOR}"
    -DMIMHID_BUILD_SHARED=ON
    -DMIMHID_BUILD_STATIC=ON
    -DMIMHID_BUILD_TESTS=OFF
    -DMIMHID_BUILD_EXAMPLES=OFF
    -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu)
if(DEFINED MIMHID_GENERATOR_PLATFORM AND
   NOT MIMHID_GENERATOR_PLATFORM STREQUAL "")
  list(APPEND _MIMHID_CONFIGURE_COMMAND -A "${MIMHID_GENERATOR_PLATFORM}")
endif()
if(DEFINED MIMHID_GENERATOR_TOOLSET AND
   NOT MIMHID_GENERATOR_TOOLSET STREQUAL "")
  list(APPEND _MIMHID_CONFIGURE_COMMAND -T "${MIMHID_GENERATOR_TOOLSET}")
endif()

execute_process(
    COMMAND ${_MIMHID_CONFIGURE_COMMAND}
    RESULT_VARIABLE _MIMHID_CONFIGURE_RESULT
    OUTPUT_VARIABLE _MIMHID_CONFIGURE_STDOUT
    ERROR_VARIABLE _MIMHID_CONFIGURE_STDERR)
if(NOT _MIMHID_CONFIGURE_RESULT EQUAL 0)
  message(FATAL_ERROR
      "nested multiarch configure failed (${_MIMHID_CONFIGURE_RESULT})\n"
      "${_MIMHID_CONFIGURE_STDOUT}\n${_MIMHID_CONFIGURE_STDERR}")
endif()

function(mimhid_require_contains contents expected label)
  string(FIND "${contents}" "${expected}" _MIMHID_MATCH_INDEX)
  if(_MIMHID_MATCH_INDEX EQUAL -1)
    message(FATAL_ERROR "${label} does not contain expected text: ${expected}")
  endif()
endfunction()

file(READ "${MIMHID_TEST_BINARY_DIR}/mimhid.pc" _MIMHID_PC)
mimhid_require_contains(
    "${_MIMHID_PC}" "prefix=\${pcfiledir}/../../.." "multiarch mimhid.pc")
mimhid_require_contains(
    "${_MIMHID_PC}"
    "libdir=\${exec_prefix}/lib/x86_64-linux-gnu"
    "multiarch mimhid.pc")
mimhid_require_contains(
    "${_MIMHID_PC}" "includedir=\${prefix}/include" "multiarch mimhid.pc")

file(READ "${MIMHID_TEST_BINARY_DIR}/mimhid-config.cmake" _MIMHID_CONFIG)
mimhid_require_contains(
    "${_MIMHID_CONFIG}"
    [=[get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../" ABSOLUTE)]=]
    "multiarch CMake package config")
# libmimhid has no external dependency, so the package config needs no library hint: the only
# multiarch-sensitive part is how deep PACKAGE_INIT climbs to the prefix.
