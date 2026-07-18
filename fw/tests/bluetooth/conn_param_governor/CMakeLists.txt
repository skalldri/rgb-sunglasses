# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(conn_param_governor_tests)

target_sources(app PRIVATE src/main.cpp)
# Only the pure decision core is under test; the wiring (bluetooth.cpp) needs
# the real BT stack + MCUmgr, neither of which this suite links.
target_sources(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bluetooth/conn_param_governor_core.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
