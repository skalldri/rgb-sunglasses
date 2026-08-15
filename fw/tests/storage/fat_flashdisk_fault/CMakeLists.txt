# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The overlay's test,fault-flash node uses this suite's local binding
# (dts/bindings) -- must be on DTS_ROOT before Zephyr is loaded.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fat_flashdisk_fault_tests)

# The patched flashdisk driver copy (fw/drivers/flashdisk). In the
# .sdk_tripwire scenario CONFIG_DISK_DRIVER_FLASH_PATCHED=n and this adds
# nothing -- the SDK driver builds instead.
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../drivers
	${CMAKE_CURRENT_BINARY_DIR}/app_drivers
)

target_sources(app PRIVATE src/main.cpp src/fault_flash.c)
