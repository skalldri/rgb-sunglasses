# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(boot_gate_tests)

target_sources(app PRIVATE src/main.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/bluetooth/boot_gate.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
