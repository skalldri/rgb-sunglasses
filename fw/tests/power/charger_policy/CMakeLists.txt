# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The overlay's ti,tps25750 / ti,bq25792 nodes use this repo's custom
# bindings (fw/dts/bindings) -- must be on DTS_ROOT before Zephyr is loaded.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(charger_policy_test)

# Real TPS25750 + BQ25792 drivers and the TPS25750 emulator — the policy is
# exercised against the real bridged register access, same DT topology as
# proto0 (bq25792 is a child of the tps25750 node).
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../drivers
	${CMAKE_CURRENT_BINARY_DIR}/app_drivers
)

# Driver public headers (zephyr/drivers/tps25750, zephyr/drivers/bq25792, ...)
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../include
	${CMAKE_CURRENT_BINARY_DIR}/app_include
)

# The module under test, compiled directly into the test app.
target_sources(app PRIVATE
	src/main.cpp
	${CMAKE_CURRENT_LIST_DIR}/../../../src/power/charger_policy.cpp
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
