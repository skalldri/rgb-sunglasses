cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bt_state_observer_tests)

target_sources(app PRIVATE
    src/main.cpp
)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
