# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(extension_tick_budget_tests)

# extension_tick_budget.h is header-only and dependency-free by design, so the
# suite compiles it straight into the test app - no extension_host.cpp (which
# needs LLEXT + USERSPACE and cannot build on native_sim).
target_sources(app PRIVATE src/main.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
