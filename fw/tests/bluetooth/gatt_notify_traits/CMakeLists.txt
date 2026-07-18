# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gatt_notify_traits_tests)

# bt_gatt_traits.h is header-only pure logic - no unit-under-test .cpp to compile,
# just the header via the include path below.
target_sources(app PRIVATE src/main.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
