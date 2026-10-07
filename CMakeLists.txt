# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.25)
project(docsuite_device_core VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

option(DOCSUITE_BUILD_DESKTOP "Build the Qt desktop application" ON)
option(DOCSUITE_BUILD_TESTS "Build tests" ON)

find_path(CUPS_INCLUDE_DIR cups/cups.h REQUIRED)
find_library(CUPS_LIBRARY NAMES cups REQUIRED)
find_path(SANE_INCLUDE_DIR sane/sane.h REQUIRED)
find_library(SANE_LIBRARY NAMES sane REQUIRED)

add_library(docsuite_device_core
    src/device/device_manager.cpp
    src/print/cups_print_backend.cpp
    src/scan/escl_scan_backend.cpp
    src/scan/sane_scan_backend.cpp
)

add_library(DocSuite::DeviceCore ALIAS docsuite_device_core)

target_include_directories(docsuite_device_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CUPS_INCLUDE_DIR}
        ${SANE_INCLUDE_DIR}
)

target_link_libraries(docsuite_device_core
    PRIVATE
        ${CUPS_LIBRARY}
        ${SANE_LIBRARY}
)

target_compile_options(docsuite_device_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(docsuite-device-cli apps/cli/main.cpp)
target_link_libraries(docsuite-device-cli PRIVATE DocSuite::DeviceCore)
set_target_properties(docsuite-device-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/cli
)

if(DOCSUITE_BUILD_DESKTOP)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Concurrent)
    qt_standard_project_setup()
    qt_add_executable(docsuite-device-center apps/desktop/main.cpp)
    target_link_libraries(
        docsuite-device-center
        PRIVATE
            DocSuite::DeviceCore
            Qt6::Widgets
            Qt6::Concurrent
    )
    set_target_properties(docsuite-device-center PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/desktop
    )
endif()

if(BUILD_TESTING AND DOCSUITE_BUILD_TESTS)
    add_executable(docsuite-core-tests tests/core_tests.cpp)
    target_link_libraries(docsuite-core-tests PRIVATE DocSuite::DeviceCore)
    add_test(NAME docsuite-core-tests COMMAND docsuite-core-tests)
endif()
