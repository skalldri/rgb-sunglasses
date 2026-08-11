# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reset_reason_tests)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/debug/reset_reason_core.c)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
