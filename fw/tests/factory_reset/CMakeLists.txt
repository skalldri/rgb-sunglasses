# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(factory_reset_tests)

target_sources(app PRIVATE src/main.cpp)
# Only the pure core logic is under test; the glue (factory_reset.cpp) needs
# the board's button/LED devicetree aliases and the real flash partitions,
# none of which exist on native_sim — same split as tests/debug/coredump_manager.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/factory_reset/factory_reset_core.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
