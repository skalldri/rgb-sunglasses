# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(battery_service_tests)

target_sources(app PRIVATE src/main.cpp)
# Unit under test. The bq25792_* driver functions it calls are stubbed in
# src/main.cpp (recording fakes) instead of compiling the real I2C driver.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/bluetooth/battery_service.cpp)
# Real persistence plumbing (same sources the settings tests exercise).
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/settings/persistent_value_registry.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/settings/persistent_value_store.cpp)

# bq25792.h public driver header lives under fw/include
add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_BINARY_DIR}/app_include
)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
# bt_service_cpp.h trips -Wcomment (known non-blocking warning, see fw/CLAUDE.md)
target_compile_options(app PRIVATE -Wno-error=comment)
