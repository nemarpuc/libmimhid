# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
cmake_minimum_required(VERSION 3.20)
project(libmimusb VERSION 0.7.0 LANGUAGES C CXX
        HOMEPAGE_URL "https://github.com/nemarpuc/libmimusb")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MIMUSB_WERROR "Treat compiler warnings as errors." ON)
option(MIMUSB_BUILD_SHARED "Build the shared library." ON)
option(MIMUSB_BUILD_STATIC "Build the static library." ON)
option(MIMUSB_BUILD_TESTS "Build tests." OFF)
option(MIMUSB_BUILD_EXAMPLES "Build examples." OFF)
option(MIMUSB_BUILD_FUZZ "Build fuzz targets (Clang only)." OFF)
option(MIMUSB_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer." OFF)
option(MIMUSB_TSAN "Enable ThreadSanitizer for race detection." OFF)
option(MIMUSB_COVERAGE "Instrument every target for gcov line and branch coverage (GCC or Clang)." OFF)

if(MIMUSB_SANITIZE AND MIMUSB_TSAN)
    message(FATAL_ERROR
        "MIMUSB_SANITIZE and MIMUSB_TSAN are mutually exclusive because "
        "ThreadSanitizer cannot be combined with AddressSanitizer.")
endif()
if(MIMUSB_TSAN)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "MIMUSB_TSAN is supported only for Linux host builds; "
            "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'.")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR
            "MIMUSB_TSAN requires GNU or Clang C and C++ compilers; found "
            "C='${CMAKE_C_COMPILER_ID}' and CXX='${CMAKE_CXX_COMPILER_ID}'.")
    endif()
    if((CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12) OR
       (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12))
        message(FATAL_ERROR
            "MIMUSB_TSAN requires GCC 12 or newer when using the GNU ThreadSanitizer runtime. "
            "GCC 11 has the unresolved condition-variable false positive tracked as GCC bug 101978.")
    endif()
    if(MIMUSB_BUILD_FUZZ)
        message(FATAL_ERROR
            "MIMUSB_TSAN cannot be combined with MIMUSB_BUILD_FUZZ because the "
            "fuzz target uses the incompatible AddressSanitizer runtime.")
    endif()

    # TSan requires a usable compiler runtime and position-independent executables. Probe both
    # languages and fail during configuration instead of leaving an obscure linker error.
    include(CheckPIESupported)
    check_pie_supported(LANGUAGES C CXX OUTPUT_VARIABLE MIMUSB_TSAN_PIE_DIAGNOSTIC)
    if(NOT CMAKE_C_LINK_PIE_SUPPORTED OR NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
        message(FATAL_ERROR
            "MIMUSB_TSAN requires PIE support for C and C++ executables. "
            "CMake's PIE probe reported: ${MIMUSB_TSAN_PIE_DIAGNOSTIC}")
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    include(CheckCSourceCompiles)
    include(CheckCXXSourceCompiles)
    set(MIMUSB_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(MIMUSB_SAVED_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS}")
    string(APPEND CMAKE_REQUIRED_FLAGS " -fsanitize=thread")
    list(APPEND CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_c_source_compiles("int main(void) { return 0; }" MIMUSB_C_TSAN_COMPILES_AND_LINKS)
    check_cxx_source_compiles("int main() { return 0; }" MIMUSB_CXX_TSAN_COMPILES_AND_LINKS)
    set(CMAKE_REQUIRED_FLAGS "${MIMUSB_SAVED_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${MIMUSB_SAVED_REQUIRED_LINK_OPTIONS}")
    if(NOT MIMUSB_C_TSAN_COMPILES_AND_LINKS OR NOT MIMUSB_CXX_TSAN_COMPILES_AND_LINKS)
        message(FATAL_ERROR
            "MIMUSB_TSAN was requested, but the selected C/C++ toolchain cannot "
            "compile and link against a usable ThreadSanitizer runtime.")
    endif()
endif()

if(MIMUSB_COVERAGE)
    if(MIMUSB_SANITIZE OR MIMUSB_TSAN)
        message(FATAL_ERROR
            "MIMUSB_COVERAGE cannot be combined with MIMUSB_SANITIZE or MIMUSB_TSAN: the sanitizer "
            "runtimes change the generated code and the counts would not describe a plain build.")
    endif()
    if(MIMUSB_BUILD_FUZZ)
        message(FATAL_ERROR
            "MIMUSB_COVERAGE cannot be combined with MIMUSB_BUILD_FUZZ, which sets its own "
            "instrumentation.")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR
            "MIMUSB_COVERAGE requires GNU or Clang C and C++ compilers; found "
            "C='${CMAKE_C_COMPILER_ID}' and CXX='${CMAKE_CXX_COMPILER_ID}'.")
    endif()
endif()

# Every target of this build, tests and examples included, carries the instrumentation: a
# sanitizer runtime cannot observe accesses in uninstrumented code. -fno-sanitize-recover makes
# the first finding fail the process instead of scrolling past.
if(MIMUSB_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(MIMUSB_TSAN)
    add_compile_options(-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
# Coverage: gcov notes and counters for every target. -O0 keeps each source line its own block;
# atomic counter updates keep the multi-threaded tests (the link's reader thread, the simulated board)
# from losing counts.
if(MIMUSB_COVERAGE)
    add_compile_options(--coverage -O0 -fprofile-update=atomic)
    add_link_options(--coverage)
endif()

if(WIN32)
    # Put the DLL next to the test and example executables so they run without PATH changes.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

set(MIMUSB_SOURCES
    src/api/c_api_device.cpp
    src/api/c_api_bytes.cpp
    src/api/c_api_endpoint.cpp
    src/api/c_api_node.cpp
    src/api/c_api_spec.cpp
    src/api/device_ops.cpp
    src/api/error_detail.cpp
    src/hid/descriptor_builder.cpp
    src/hid/item_writer.cpp
    src/hid/validator.cpp
    src/classes/cdc_acm.cpp
    src/classes/interfaces.cpp
    src/classes/hid/hid_class.cpp
    src/classes/hid/hid_layer.cpp
    src/classes/hid/neutral.cpp
    src/classes/hid/report.cpp
    src/classes/hid/send.cpp
    src/classes/hid/spec_gamepad.cpp
    src/classes/hid/spec_keyboard.cpp
    src/classes/hid/spec_mouse.cpp
    src/classes/hid/spec_pen.cpp
    src/classes/hid/spec_raw.cpp
    src/classes/hid/spec_toggle.cpp
    src/classes/hid/spec_touch.cpp
    src/classes/hid/state_battery.cpp
    src/classes/hid/state_gamepad.cpp
    src/classes/hid/state_keyboard.cpp
    src/classes/hid/state_mouse.cpp
    src/classes/hid/state_pen.cpp
    src/classes/hid/state_toggle.cpp
    src/classes/hid/state_touch.cpp
    src/transport/frame.cpp
    src/transport/link.cpp
    src/usb/descriptor_composer.cpp
    src/usb/descriptor_walker.cpp
    src/transport/handshaker.cpp
)
if(WIN32)
    list(APPEND MIMUSB_SOURCES src/transport/serial_win32.cpp src/transport/ports_win32.cpp)
elseif(APPLE)
    list(APPEND MIMUSB_SOURCES src/transport/serial_posix.cpp src/transport/ports_macos.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND MIMUSB_SOURCES src/transport/serial_posix.cpp src/transport/serial_baud_linux.cpp
         src/transport/ports_linux.cpp)
else()
    message(FATAL_ERROR "libmimusb has no serial backend for ${CMAKE_SYSTEM_NAME}")
endif()

find_package(Threads REQUIRED)

# The shared library and the test executables use separately compiled object files: on Windows
# MIMUSB_BUILDING_LIBRARY expands MIMUSB_API to __declspec(dllexport), and the tests need the
# internal symbols the DLL does not export.
function(mimusb_add_objects target export_api)
    add_library(${target} OBJECT ${MIMUSB_SOURCES})
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE YES)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/proto)
    if(export_api)
        target_compile_definitions(${target} PRIVATE MIMUSB_BUILDING_LIBRARY=1)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /GR- /utf-8)
        if(MIMUSB_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            -fno-rtti -fvisibility=hidden)
        if(MIMUSB_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(NOT MIMUSB_BUILD_SHARED AND NOT MIMUSB_BUILD_STATIC)
    message(FATAL_ERROR "Enable at least one of MIMUSB_BUILD_SHARED and MIMUSB_BUILD_STATIC.")
endif()

# C++ runtime that static consumers of the C ABI must link.
set(MIMUSB_CXX_RUNTIME_LIBRARY "")
if(MIMUSB_BUILD_STATIC AND NOT MSVC AND NOT WIN32)
    foreach(candidate IN ITEMS stdc++ c++)
        if(MIMUSB_CXX_RUNTIME_LIBRARY STREQUAL "")
            list(FIND CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "${candidate}" index)
            if(NOT index EQUAL -1)
                set(MIMUSB_CXX_RUNTIME_LIBRARY "${candidate}")
            endif()
        endif()
    endforeach()
    if(MIMUSB_CXX_RUNTIME_LIBRARY STREQUAL "")
        message(FATAL_ERROR "Cannot derive the C++ runtime from CMAKE_CXX_IMPLICIT_LINK_LIBRARIES.")
    endif()
endif()

if(MIMUSB_BUILD_SHARED)
    mimusb_add_objects(mimusb_shared_objects TRUE)
    add_library(mimusb SHARED $<TARGET_OBJECTS:mimusb_shared_objects>)
    add_library(mimusb::shared ALIAS mimusb)
    target_include_directories(mimusb PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(mimusb PRIVATE Threads::Threads)
    set_target_properties(mimusb PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
    if(WIN32)
        target_link_libraries(mimusb PRIVATE setupapi advapi32)
        target_compile_definitions(mimusb INTERFACE MIMUSB_SHARED=1)
        if(MINGW)
            # Keep the DLL free of libgcc/libstdc++/winpthread runtime DLL dependencies.
            target_link_options(mimusb PRIVATE -static)
        endif()
    elseif(APPLE)
        set(MIMUSB_APPLE_EXPORT_LIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mimusb.exports")
        target_link_options(mimusb PRIVATE "LINKER:-exported_symbols_list,${MIMUSB_APPLE_EXPORT_LIST}")
        set_property(TARGET mimusb APPEND PROPERTY LINK_DEPENDS "${MIMUSB_APPLE_EXPORT_LIST}")
    else()
        # The ABI is plain C: the C++ runtime stays private to the shared object.
        set(MIMUSB_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mimusb.map")
        if(NOT MIMUSB_SANITIZE AND NOT MIMUSB_TSAN)
            target_link_options(mimusb PRIVATE -static-libstdc++ -static-libgcc)
        endif()
        target_link_options(mimusb PRIVATE
            "-Wl,--exclude-libs,ALL" "-Wl,--version-script=${MIMUSB_VERSION_SCRIPT}")
        set_property(TARGET mimusb APPEND PROPERTY LINK_DEPENDS "${MIMUSB_VERSION_SCRIPT}")
    endif()
endif()

if(MIMUSB_BUILD_STATIC)
    mimusb_add_objects(mimusb_static_objects FALSE)
    add_library(mimusb_static STATIC $<TARGET_OBJECTS:mimusb_static_objects>)
    add_library(mimusb::static ALIAS mimusb_static)
    target_include_directories(mimusb_static PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(WIN32)
        target_link_libraries(mimusb_static INTERFACE setupapi advapi32)
        set_target_properties(mimusb_static PROPERTIES OUTPUT_NAME mimusb_static)
    else()
        target_link_libraries(mimusb_static INTERFACE "$<LINK_ONLY:pthread>"
            "$<LINK_ONLY:${MIMUSB_CXX_RUNTIME_LIBRARY}>")
        if(MIMUSB_BUILD_SHARED)
            set_target_properties(mimusb_static PROPERTIES OUTPUT_NAME mimusb_static)
        else()
            set_target_properties(mimusb_static PROPERTIES OUTPUT_NAME mimusb)
        endif()
    endif()
    # The installed name stays the same for static-only and combined packages.
    set_target_properties(mimusb_static PROPERTIES EXPORT_NAME mimusb_static)
endif()

set(MIMUSB_PRIMARY_TARGET mimusb_static)
if(MIMUSB_BUILD_SHARED)
    set(MIMUSB_PRIMARY_TARGET mimusb)
endif()
add_library(mimusb::mimusb ALIAS ${MIMUSB_PRIMARY_TARGET})

# The stable C ABI remains usable from a project that enables only C. The separate C++ facade
# carries the language-level requirement of mimusb.hpp without making such pure-C consumers enable
# CXX merely to link the library.
if(MIMUSB_BUILD_SHARED)
    add_library(mimusb_cpp INTERFACE)
    add_library(mimusb::cpp ALIAS mimusb_cpp)
    target_link_libraries(mimusb_cpp INTERFACE mimusb)
    target_compile_features(mimusb_cpp INTERFACE cxx_std_20)
    set_target_properties(mimusb_cpp PROPERTIES EXPORT_NAME cpp)
endif()
if(MIMUSB_BUILD_STATIC)
    if(MIMUSB_BUILD_SHARED)
        add_library(mimusb_cpp_static INTERFACE)
        add_library(mimusb::cpp_static ALIAS mimusb_cpp_static)
        target_link_libraries(mimusb_cpp_static INTERFACE mimusb_static)
        target_compile_features(mimusb_cpp_static INTERFACE cxx_std_20)
        set_target_properties(mimusb_cpp_static PROPERTIES EXPORT_NAME cpp_static)
    else()
        add_library(mimusb_cpp INTERFACE)
        add_library(mimusb::cpp ALIAS mimusb_cpp)
        target_link_libraries(mimusb_cpp INTERFACE mimusb_static)
        target_compile_features(mimusb_cpp INTERFACE cxx_std_20)
        set_target_properties(mimusb_cpp PROPERTIES EXPORT_NAME cpp)
    endif()
endif()

if(MIMUSB_BUILD_TESTS)
    include(CTest)
    mimusb_add_objects(mimusb_test_objects FALSE)
    include(cmake/mimusb-tests.cmake)
endif()

if(MIMUSB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MIMUSB_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MIMUSB_BUILD_FUZZ needs Clang (libFuzzer).")
    endif()
    if(MIMUSB_SANITIZE)
        message(FATAL_ERROR "MIMUSB_BUILD_FUZZ sets its own sanitizers; leave MIMUSB_SANITIZE off.")
    endif()
    mimusb_add_objects(mimusb_fuzz_objects FALSE)
    target_compile_options(mimusb_fuzz_objects PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
    add_executable(mimusb_fuzz_descriptor
        tests/fuzz/fuzz_descriptor.cpp
        $<TARGET_OBJECTS:mimusb_fuzz_objects>)
    set_target_properties(mimusb_fuzz_descriptor PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_include_directories(mimusb_fuzz_descriptor PRIVATE include src proto)
    target_compile_options(mimusb_fuzz_descriptor PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(mimusb_fuzz_descriptor PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(mimusb_fuzz_descriptor PRIVATE Threads::Threads)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(MIMUSB_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/mimusb")
set(MIMUSB_PC_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(MIMUSB_BUILD_SHARED)
    install(TARGETS mimusb mimusb_cpp EXPORT mimusb-shared-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT mimusb-shared-targets FILE mimusb-shared-targets.cmake
        NAMESPACE mimusb:: DESTINATION ${MIMUSB_CMAKE_INSTALL_DIR})
endif()
if(MIMUSB_BUILD_STATIC)
    set(MIMUSB_STATIC_INSTALL_TARGETS mimusb_static)
    if(MIMUSB_BUILD_SHARED)
        list(APPEND MIMUSB_STATIC_INSTALL_TARGETS mimusb_cpp_static)
    else()
        list(APPEND MIMUSB_STATIC_INSTALL_TARGETS mimusb_cpp)
    endif()
    install(TARGETS ${MIMUSB_STATIC_INSTALL_TARGETS} EXPORT mimusb-static-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT mimusb-static-targets FILE mimusb-static-targets.cmake
        NAMESPACE mimusb:: DESTINATION ${MIMUSB_CMAKE_INSTALL_DIR})
endif()

install(FILES include/mimusb.h include/mimusb.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libmimusb)

configure_package_config_file(cmake/mimusb-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mimusb-config.cmake
    INSTALL_DESTINATION ${MIMUSB_CMAKE_INSTALL_DIR})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/mimusb-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mimusb-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mimusb-config-version.cmake
    DESTINATION ${MIMUSB_CMAKE_INSTALL_DIR})

# pkg-config: the prefix is derived from the actual pkgconfig directory depth.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(MIMUSB_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(MIMUSB_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
    file(RELATIVE_PATH MIMUSB_PC_PREFIX_FROM_PCDIR
        "${CMAKE_INSTALL_PREFIX}/${MIMUSB_PC_INSTALL_DIR}" "${CMAKE_INSTALL_PREFIX}")
    string(REGEX REPLACE "/+$" "" MIMUSB_PC_PREFIX_FROM_PCDIR "${MIMUSB_PC_PREFIX_FROM_PCDIR}")
    set(MIMUSB_PC_PREFIX "\${pcfiledir}/${MIMUSB_PC_PREFIX_FROM_PCDIR}")
    set(MIMUSB_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(MIMUSB_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(MIMUSB_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

set(MIMUSB_PC_FILES "")
if(MIMUSB_BUILD_SHARED)
    set(MIMUSB_PC_NAME "libmimusb")
    set(MIMUSB_PC_LIBRARY_NAME mimusb)
    set(MIMUSB_PC_LIBS_PRIVATE "")
    configure_file(cmake/mimusb.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mimusb.pc @ONLY)
    list(APPEND MIMUSB_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/mimusb.pc)
endif()
if(MIMUSB_BUILD_STATIC)
    set(MIMUSB_PC_LIBRARY_NAME mimusb)
    if(WIN32 OR MIMUSB_BUILD_SHARED)
        set(MIMUSB_PC_LIBRARY_NAME mimusb_static)
    endif()
    if(WIN32)
        set(MIMUSB_PC_LIBS_PRIVATE "Libs.private: -lsetupapi -ladvapi32")
    else()
        set(MIMUSB_PC_LIBS_PRIVATE "Libs.private: -l${MIMUSB_CXX_RUNTIME_LIBRARY} -lpthread")
    endif()
    if(MIMUSB_BUILD_SHARED)
        set(MIMUSB_PC_NAME "libmimusb static")
        configure_file(cmake/mimusb.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mimusb-static.pc @ONLY)
        list(APPEND MIMUSB_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/mimusb-static.pc)
    else()
        set(MIMUSB_PC_NAME "libmimusb")
        configure_file(cmake/mimusb.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mimusb.pc @ONLY)
        list(APPEND MIMUSB_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/mimusb.pc)
    endif()
endif()
install(FILES ${MIMUSB_PC_FILES} DESTINATION ${MIMUSB_PC_INSTALL_DIR})
