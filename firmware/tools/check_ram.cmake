# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
# mimusb_check_ram(<target> <ram bytes of the chip> <reserve bytes> [<bytes outside data+bss>])
# After the link, runs tools/check_ram.py on the image (see that file). The `size` tool is the one of
# the compiler's own toolchain (arm-none-eabi-gcc -> arm-none-eabi-size).
# (not CMAKE_CURRENT_FUNCTION_LIST_DIR: that needs CMake 3.17, and the firmware asks for 3.13)
set(MIMUSB_CHECK_RAM_DIR ${CMAKE_CURRENT_LIST_DIR})
function(mimusb_check_ram target ram reserve)
    set(extra ${ARGN})
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(CMAKE_C_COMPILER MATCHES "gcc\\.exe$")
        string(REGEX REPLACE "gcc\\.exe$" "size.exe" size_tool "${CMAKE_C_COMPILER}")
    elseif(CMAKE_C_COMPILER MATCHES "gcc$")
        string(REGEX REPLACE "gcc$" "size" size_tool "${CMAKE_C_COMPILER}")
    else()
        message(FATAL_ERROR "mimusb_check_ram: the C compiler '${CMAKE_C_COMPILER}' is not a gcc, so "
                            "its `size` tool is not known")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${MIMUSB_CHECK_RAM_DIR}/check_ram.py
                ${size_tool} $<TARGET_FILE:${target}> ${ram} ${reserve} ${extra}
        VERBATIM)
endfunction()
