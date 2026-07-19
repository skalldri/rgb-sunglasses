# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The overlay's ti,tps25750 / ti,bq25792 nodes use this repo's custom
# bindings (fw/dts/bindings) -- must be on DTS_ROOT before Zephyr is loaded.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(power_shell_test)

# Real TPS25750 + BQ25792 drivers and the TPS25750 emulator, same as
# fw/tests/drivers/emul_tps25750 (this suite reuses that stack, not a second
# emulator setup).
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

# Unit under test.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/power.cpp)

# charger_status_thread_func (unconditionally started whenever CONFIG_BQ25792=y)
# calls into this - Zephyr-free, already covered by its own tests/power/comm_health
# suite.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/power/comm_health.cpp)

# Satisfies tps25750_get_patch()'s CONFIG_TPS25750_INTERNAL_PATCH definition
# (see prj.conf) so cmd_power_pd_patch compiles/links - not otherwise
# exercised by this suite.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../misc/tps25750/tps25750-config-compressed.c)
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../../misc)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
