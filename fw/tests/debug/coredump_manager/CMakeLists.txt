# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coredump_manager_tests)

target_sources(app PRIVATE src/main.cpp)
# Only the pure core logic is under test; the glue (coredump_manager.cpp) needs
# CONFIG_DEBUG_COREDUMP, which doesn't exist on native_sim/posix.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/debug/coredump_manager_core.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
