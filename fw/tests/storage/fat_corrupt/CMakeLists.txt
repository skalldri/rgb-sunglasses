# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fat_corrupt_tests)

# The patched flashdisk driver copy (fw/drivers/flashdisk, issue #380).
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../drivers
	${CMAKE_CURRENT_BINARY_DIR}/app_drivers
)

target_sources(app PRIVATE src/main.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/storage/storage.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
