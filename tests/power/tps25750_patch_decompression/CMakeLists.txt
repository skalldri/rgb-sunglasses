# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(integration)

# Need the TPS25752 driver
add_subdirectory(
	${CMAKE_CURRENT_LIST_DIR}/../../../drivers
	${CMAKE_CURRENT_BINARY_DIR}/app_drivers
)

# Need TPS25750 includes
add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_BINARY_DIR}/app_include
)

target_sources(app PRIVATE src/main.cpp)

# Include the original patch
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../misc/tps25750/tps25750-config.c)
# Include the compressed patch
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../misc/tps25750/tps25750-config-compressed.c)

# Include power management source code
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/power.cpp)

# Include app and driver headers
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../misc)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../drivers)

# Needs to be accessible to drivers so that the TPS25750 can access the patch file
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../../misc)