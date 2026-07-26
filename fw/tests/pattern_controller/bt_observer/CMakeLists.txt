# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pattern_controller_bt_observer_test)

target_sources(app PRIVATE src/main.cpp)

# Unit under test.
target_sources(app PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/../../../src/pattern_controller_bt_observer.cpp
)

# mocks/ MUST precede src/ so mocks/animations/bt_animations.h wins over the real
# header (which pulls in the registry/renderer/font atlas this suite has no reason
# to build). pattern_controller.cpp, bluetooth.cpp and status_led.cpp are not
# linked either — their observer-visible functions are faked in src/main.cpp.
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/mocks
	${CMAKE_CURRENT_LIST_DIR}/../../../src
)
