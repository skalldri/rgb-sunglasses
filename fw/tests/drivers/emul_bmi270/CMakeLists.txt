# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(emul_bmi270_test)

# Need the out-of-tree BMI270 emulator (fw/drivers/emul_bmi270)
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../drivers
	${CMAKE_CURRENT_BINARY_DIR}/app_drivers
)

target_sources(app PRIVATE src/main.c)
