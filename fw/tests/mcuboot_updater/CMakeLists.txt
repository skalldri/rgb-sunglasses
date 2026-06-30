# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mcuboot_updater_tests)

# Mock pm_config.h must come before any generated or system headers.
target_include_directories(app BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)

target_sources(app PRIVATE src/main.cpp)
target_sources(app PRIVATE src/retention_stubs.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mcuboot_updater.cpp)
