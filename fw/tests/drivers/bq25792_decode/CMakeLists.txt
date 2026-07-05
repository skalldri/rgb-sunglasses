# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bq25792_decode_tests)

target_sources(app PRIVATE src/main.cpp)

# InternalDeviceRegister.hpp lives under fw/include (zephyr/drivers/...)
add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_BINARY_DIR}/app_include
)

# bq25792_priv.h (the register map + unit-conversion classes under test)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../drivers)
