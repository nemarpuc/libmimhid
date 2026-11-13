# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
# mimusb_check_ram(<target> <ram bytes of the chip> <reserve bytes> [<bytes outside data+bss>])
# After the link, runs tools/check_ram.py on the image (see that file). The `size` tool is the one of
# the compiler's own toolchain (arm-none-eabi-gcc -> arm-none-eabi-size).
function(mimusb_check_ram target ram reserve)
    set(extra ${ARGN})
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    string(REGEX REPLACE "gcc(\\.exe)?$" "size\\1" size_tool "${CMAKE_C_COMPILER}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_ram.py
                ${size_tool} $<TARGET_FILE:${target}> ${ram} ${reserve} ${extra}
        VERBATIM)
endfunction()
