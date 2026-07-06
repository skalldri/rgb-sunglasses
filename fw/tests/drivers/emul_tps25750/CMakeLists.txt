# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The overlay's ti,tps25750 / ti,bq25792 nodes use this repo's custom
# bindings (fw/dts/bindings) -- must be on DTS_ROOT before Zephyr is loaded.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(emul_tps25750_test)

# Real TPS25750 + BQ25792 drivers and the TPS25750 emulator
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../drivers
	${CMAKE_CURRENT_BINARY_DIR}/app_drivers
)

# Driver public headers (zephyr/drivers/tps25750, zephyr/drivers/bq25792, ...)
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../include
	${CMAKE_CURRENT_BINARY_DIR}/app_include
)

target_sources(app PRIVATE src/main.cpp)

# The uncompressed patch blob (rodata; only actually consumed by the
# .patch_download scenario, where the driver's tps25750_get_patch() serves it
# via CONFIG_TPS25750_PATCH_HEADER_FILE)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../misc/tps25750/tps25750-config.c)

# Needs to be accessible to drivers so that the TPS25750 can access the patch file
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../../misc)
