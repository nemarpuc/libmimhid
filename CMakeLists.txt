# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
cmake_minimum_required(VERSION 3.20)
project(libmimhid VERSION 0.2.0 LANGUAGES C CXX
        HOMEPAGE_URL "https://github.com/nemarpuc/libmimhid")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MIMHID_WERROR "Treat compiler warnings as errors." ON)
option(MIMHID_BUILD_SHARED "Build the shared library." ON)
option(MIMHID_BUILD_STATIC "Build the static library." ON)
option(MIMHID_BUILD_TESTS "Build tests." OFF)
option(MIMHID_BUILD_EXAMPLES "Build examples." OFF)
option(MIMHID_BUILD_FUZZ "Build fuzz targets (Clang only)." OFF)
option(MIMHID_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer." OFF)
option(MIMHID_TSAN "Enable ThreadSanitizer for race detection." OFF)

if(MIMHID_SANITIZE AND MIMHID_TSAN)
    message(FATAL_ERROR
        "MIMHID_SANITIZE and MIMHID_TSAN are mutually exclusive because "
        "ThreadSanitizer cannot be combined with AddressSanitizer.")
endif()
if(MIMHID_TSAN)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "MIMHID_TSAN is supported only for Linux host builds; "
            "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'.")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR
            "MIMHID_TSAN requires GNU or Clang C and C++ compilers; found "
            "C='${CMAKE_C_COMPILER_ID}' and CXX='${CMAKE_CXX_COMPILER_ID}'.")
    endif()
    if((CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12) OR
       (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12))
        message(FATAL_ERROR
            "MIMHID_TSAN requires GCC 12 or newer when using the GNU ThreadSanitizer runtime. "
            "GCC 11 has the unresolved condition-variable false positive tracked as GCC bug 101978.")
    endif()
    if(MIMHID_BUILD_FUZZ)
        message(FATAL_ERROR
            "MIMHID_TSAN cannot be combined with MIMHID_BUILD_FUZZ because the "
            "fuzz target uses the incompatible AddressSanitizer runtime.")
    endif()

    # TSan requires a usable compiler runtime and position-independent executables. Probe both
    # languages and fail during configuration instead of leaving an obscure linker error.
    include(CheckPIESupported)
    check_pie_supported(LANGUAGES C CXX OUTPUT_VARIABLE MIMHID_TSAN_PIE_DIAGNOSTIC)
    if(NOT CMAKE_C_LINK_PIE_SUPPORTED OR NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
        message(FATAL_ERROR
            "MIMHID_TSAN requires PIE support for C and C++ executables. "
            "CMake's PIE probe reported: ${MIMHID_TSAN_PIE_DIAGNOSTIC}")
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    include(CheckCSourceCompiles)
    include(CheckCXXSourceCompiles)
    set(MIMHID_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(MIMHID_SAVED_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS}")
    string(APPEND CMAKE_REQUIRED_FLAGS " -fsanitize=thread")
    list(APPEND CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_c_source_compiles("int main(void) { return 0; }" MIMHID_C_TSAN_COMPILES_AND_LINKS)
    check_cxx_source_compiles("int main() { return 0; }" MIMHID_CXX_TSAN_COMPILES_AND_LINKS)
    set(CMAKE_REQUIRED_FLAGS "${MIMHID_SAVED_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${MIMHID_SAVED_REQUIRED_LINK_OPTIONS}")
    if(NOT MIMHID_C_TSAN_COMPILES_AND_LINKS OR NOT MIMHID_CXX_TSAN_COMPILES_AND_LINKS)
        message(FATAL_ERROR
            "MIMHID_TSAN was requested, but the selected C/C++ toolchain cannot "
            "compile and link against a usable ThreadSanitizer runtime.")
    endif()
endif()

# Every target of this build, tests and examples included, carries the instrumentation: a
# sanitizer runtime cannot observe accesses in uninstrumented code. -fno-sanitize-recover makes
# the first finding fail the process instead of scrolling past.
if(MIMHID_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(MIMHID_TSAN)
    add_compile_options(-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if(WIN32)
    # Put the DLL next to the test and example executables so they run without PATH changes.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

set(MIMHID_SOURCES
    src/api/c_api_device.cpp
    src/api/c_api_node.cpp
    src/api/c_api_send.cpp
    src/api/device_ops.cpp
    src/api/error_detail.cpp
    src/hid/descriptor_builder.cpp
    src/hid/item_writer.cpp
    src/hid/validator.cpp
    src/profiles/spec.cpp
    src/profiles/state.cpp
    src/transport/frame.cpp
    src/transport/link.cpp
    src/transport/handshaker.cpp
)
if(WIN32)
    list(APPEND MIMHID_SOURCES src/transport/serial_win32.cpp src/transport/ports_win32.cpp)
elseif(APPLE)
    list(APPEND MIMHID_SOURCES src/transport/serial_posix.cpp src/transport/ports_macos.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND MIMHID_SOURCES src/transport/serial_posix.cpp src/transport/serial_baud_linux.cpp
         src/transport/ports_linux.cpp)
else()
    message(FATAL_ERROR "libmimhid has no serial backend for ${CMAKE_SYSTEM_NAME}")
endif()

find_package(Threads REQUIRED)

# The shared library and the test executables use separately compiled object files: on Windows
# MIMHID_BUILDING_LIBRARY expands MIMHID_API to __declspec(dllexport), and the tests need the
# internal symbols the DLL does not export.
function(mimhid_add_objects target export_api)
    add_library(${target} OBJECT ${MIMHID_SOURCES})
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE YES)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/proto)
    if(export_api)
        target_compile_definitions(${target} PRIVATE MIMHID_BUILDING_LIBRARY=1)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /GR- /utf-8)
        if(MIMHID_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            -fno-rtti -fvisibility=hidden)
        if(MIMHID_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(NOT MIMHID_BUILD_SHARED AND NOT MIMHID_BUILD_STATIC)
    message(FATAL_ERROR "Enable at least one of MIMHID_BUILD_SHARED and MIMHID_BUILD_STATIC.")
endif()

# C++ runtime that static consumers of the C ABI must link.
set(MIMHID_CXX_RUNTIME_LIBRARY "")
if(MIMHID_BUILD_STATIC AND NOT MSVC AND NOT WIN32)
    foreach(candidate IN ITEMS stdc++ c++)
        if(MIMHID_CXX_RUNTIME_LIBRARY STREQUAL "")
            list(FIND CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "${candidate}" index)
            if(NOT index EQUAL -1)
                set(MIMHID_CXX_RUNTIME_LIBRARY "${candidate}")
            endif()
        endif()
    endforeach()
    if(MIMHID_CXX_RUNTIME_LIBRARY STREQUAL "")
        message(FATAL_ERROR "Cannot derive the C++ runtime from CMAKE_CXX_IMPLICIT_LINK_LIBRARIES.")
    endif()
endif()

if(MIMHID_BUILD_SHARED)
    mimhid_add_objects(mimhid_shared_objects TRUE)
    add_library(mimhid SHARED $<TARGET_OBJECTS:mimhid_shared_objects>)
    add_library(mimhid::shared ALIAS mimhid)
    target_include_directories(mimhid PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(mimhid PRIVATE Threads::Threads)
    set_target_properties(mimhid PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
    if(WIN32)
        target_link_libraries(mimhid PRIVATE setupapi advapi32)
        target_compile_definitions(mimhid INTERFACE MIMHID_SHARED=1)
        if(MINGW)
            # Keep the DLL free of libgcc/libstdc++/winpthread runtime DLL dependencies.
            target_link_options(mimhid PRIVATE -static)
        endif()
    elseif(APPLE)
        set(MIMHID_APPLE_EXPORT_LIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mimhid.exports")
        target_link_options(mimhid PRIVATE "LINKER:-exported_symbols_list,${MIMHID_APPLE_EXPORT_LIST}")
        set_property(TARGET mimhid APPEND PROPERTY LINK_DEPENDS "${MIMHID_APPLE_EXPORT_LIST}")
    else()
        # The ABI is plain C: the C++ runtime stays private to the shared object.
        set(MIMHID_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mimhid.map")
        if(NOT MIMHID_SANITIZE AND NOT MIMHID_TSAN)
            target_link_options(mimhid PRIVATE -static-libstdc++ -static-libgcc)
        endif()
        target_link_options(mimhid PRIVATE
            "-Wl,--exclude-libs,ALL" "-Wl,--version-script=${MIMHID_VERSION_SCRIPT}")
        set_property(TARGET mimhid APPEND PROPERTY LINK_DEPENDS "${MIMHID_VERSION_SCRIPT}")
    endif()
endif()

if(MIMHID_BUILD_STATIC)
    mimhid_add_objects(mimhid_static_objects FALSE)
    add_library(mimhid_static STATIC $<TARGET_OBJECTS:mimhid_static_objects>)
    add_library(mimhid::static ALIAS mimhid_static)
    target_include_directories(mimhid_static PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(WIN32)
        target_link_libraries(mimhid_static INTERFACE setupapi advapi32)
        set_target_properties(mimhid_static PROPERTIES OUTPUT_NAME mimhid_static)
    else()
        target_link_libraries(mimhid_static INTERFACE "$<LINK_ONLY:pthread>"
            "$<LINK_ONLY:${MIMHID_CXX_RUNTIME_LIBRARY}>")
        if(MIMHID_BUILD_SHARED)
            set_target_properties(mimhid_static PROPERTIES OUTPUT_NAME mimhid_static)
        else()
            set_target_properties(mimhid_static PROPERTIES OUTPUT_NAME mimhid)
        endif()
    endif()
    # The installed name stays the same for static-only and combined packages.
    set_target_properties(mimhid_static PROPERTIES EXPORT_NAME mimhid_static)
endif()

set(MIMHID_PRIMARY_TARGET mimhid_static)
if(MIMHID_BUILD_SHARED)
    set(MIMHID_PRIMARY_TARGET mimhid)
endif()
add_library(mimhid::mimhid ALIAS ${MIMHID_PRIMARY_TARGET})

# The stable C ABI remains usable from a project that enables only C. The separate C++ facade
# carries the language-level requirement of mimhid.hpp without making such pure-C consumers enable
# CXX merely to link the library.
if(MIMHID_BUILD_SHARED)
    add_library(mimhid_cpp INTERFACE)
    add_library(mimhid::cpp ALIAS mimhid_cpp)
    target_link_libraries(mimhid_cpp INTERFACE mimhid)
    target_compile_features(mimhid_cpp INTERFACE cxx_std_20)
    set_target_properties(mimhid_cpp PROPERTIES EXPORT_NAME cpp)
endif()
if(MIMHID_BUILD_STATIC)
    if(MIMHID_BUILD_SHARED)
        add_library(mimhid_cpp_static INTERFACE)
        add_library(mimhid::cpp_static ALIAS mimhid_cpp_static)
        target_link_libraries(mimhid_cpp_static INTERFACE mimhid_static)
        target_compile_features(mimhid_cpp_static INTERFACE cxx_std_20)
        set_target_properties(mimhid_cpp_static PROPERTIES EXPORT_NAME cpp_static)
    else()
        add_library(mimhid_cpp INTERFACE)
        add_library(mimhid::cpp ALIAS mimhid_cpp)
        target_link_libraries(mimhid_cpp INTERFACE mimhid_static)
        target_compile_features(mimhid_cpp INTERFACE cxx_std_20)
        set_target_properties(mimhid_cpp PROPERTIES EXPORT_NAME cpp)
    endif()
endif()

if(MIMHID_BUILD_TESTS)
    include(CTest)
    mimhid_add_objects(mimhid_test_objects FALSE)
    include(cmake/mimhid-tests.cmake)
endif()

if(MIMHID_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MIMHID_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MIMHID_BUILD_FUZZ needs Clang (libFuzzer).")
    endif()
    if(MIMHID_SANITIZE)
        message(FATAL_ERROR "MIMHID_BUILD_FUZZ sets its own sanitizers; leave MIMHID_SANITIZE off.")
    endif()
    mimhid_add_objects(mimhid_fuzz_objects FALSE)
    target_compile_options(mimhid_fuzz_objects PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
    add_executable(mimhid_fuzz_descriptor
        tests/fuzz/fuzz_descriptor.cpp
        $<TARGET_OBJECTS:mimhid_fuzz_objects>)
    set_target_properties(mimhid_fuzz_descriptor PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_include_directories(mimhid_fuzz_descriptor PRIVATE include src proto)
    target_compile_options(mimhid_fuzz_descriptor PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(mimhid_fuzz_descriptor PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(mimhid_fuzz_descriptor PRIVATE Threads::Threads)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(MIMHID_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/mimhid")
set(MIMHID_PC_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(MIMHID_BUILD_SHARED)
    install(TARGETS mimhid mimhid_cpp EXPORT mimhid-shared-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT mimhid-shared-targets FILE mimhid-shared-targets.cmake
        NAMESPACE mimhid:: DESTINATION ${MIMHID_CMAKE_INSTALL_DIR})
endif()
if(MIMHID_BUILD_STATIC)
    set(MIMHID_STATIC_INSTALL_TARGETS mimhid_static)
    if(MIMHID_BUILD_SHARED)
        list(APPEND MIMHID_STATIC_INSTALL_TARGETS mimhid_cpp_static)
    else()
        list(APPEND MIMHID_STATIC_INSTALL_TARGETS mimhid_cpp)
    endif()
    install(TARGETS ${MIMHID_STATIC_INSTALL_TARGETS} EXPORT mimhid-static-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT mimhid-static-targets FILE mimhid-static-targets.cmake
        NAMESPACE mimhid:: DESTINATION ${MIMHID_CMAKE_INSTALL_DIR})
endif()

install(FILES include/mimhid.h include/mimhid.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libmimhid)

configure_package_config_file(cmake/mimhid-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mimhid-config.cmake
    INSTALL_DESTINATION ${MIMHID_CMAKE_INSTALL_DIR})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/mimhid-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mimhid-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mimhid-config-version.cmake
    DESTINATION ${MIMHID_CMAKE_INSTALL_DIR})

# pkg-config: the prefix is derived from the actual pkgconfig directory depth.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(MIMHID_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(MIMHID_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
    file(RELATIVE_PATH MIMHID_PC_PREFIX_FROM_PCDIR
        "${CMAKE_INSTALL_PREFIX}/${MIMHID_PC_INSTALL_DIR}" "${CMAKE_INSTALL_PREFIX}")
    string(REGEX REPLACE "/+$" "" MIMHID_PC_PREFIX_FROM_PCDIR "${MIMHID_PC_PREFIX_FROM_PCDIR}")
    set(MIMHID_PC_PREFIX "\${pcfiledir}/${MIMHID_PC_PREFIX_FROM_PCDIR}")
    set(MIMHID_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(MIMHID_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(MIMHID_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

set(MIMHID_PC_FILES "")
if(MIMHID_BUILD_SHARED)
    set(MIMHID_PC_NAME "libmimhid")
    set(MIMHID_PC_LIBRARY_NAME mimhid)
    set(MIMHID_PC_LIBS_PRIVATE "")
    configure_file(cmake/mimhid.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mimhid.pc @ONLY)
    list(APPEND MIMHID_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/mimhid.pc)
endif()
if(MIMHID_BUILD_STATIC)
    set(MIMHID_PC_LIBRARY_NAME mimhid)
    if(WIN32 OR MIMHID_BUILD_SHARED)
        set(MIMHID_PC_LIBRARY_NAME mimhid_static)
    endif()
    if(WIN32)
        set(MIMHID_PC_LIBS_PRIVATE "Libs.private: -lsetupapi -ladvapi32")
    else()
        set(MIMHID_PC_LIBS_PRIVATE "Libs.private: -l${MIMHID_CXX_RUNTIME_LIBRARY} -lpthread")
    endif()
    if(MIMHID_BUILD_SHARED)
        set(MIMHID_PC_NAME "libmimhid static")
        configure_file(cmake/mimhid.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mimhid-static.pc @ONLY)
        list(APPEND MIMHID_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/mimhid-static.pc)
    else()
        set(MIMHID_PC_NAME "libmimhid")
        configure_file(cmake/mimhid.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mimhid.pc @ONLY)
        list(APPEND MIMHID_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/mimhid.pc)
    endif()
endif()
install(FILES ${MIMHID_PC_FILES} DESTINATION ${MIMHID_PC_INSTALL_DIR})
