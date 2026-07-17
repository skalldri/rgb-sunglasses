cmake_minimum_required(VERSION 3.20.0)

# Custom DT binding (test,fake-led-strip) lives in this test's dts/bindings —
# DTS_ROOT must be appended BEFORE find_package(Zephyr) or the binding is
# silently not found.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(led_controller_test)

target_sources(app PRIVATE
    src/main.cpp
    src/fake_led_strip.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/led_controller.cpp
)

# The stub core_config.h in include/ must shadow fw/src's real one (which drags
# in the BT GATT stack) — BEFORE is required for include-path precedence; see
# fw/tests/mcuboot_updater/CMakeLists.txt.
target_include_directories(app BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
