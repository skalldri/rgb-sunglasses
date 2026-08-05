# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(core_config_tests)

target_sources(app PRIVATE src/main.cpp)
# Unit under test: the Core Config service, including the appended
# "Active Animation" characteristic and its ActiveAnimationBinding registrar.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/core_config.cpp)
# Real persistence plumbing (the four config characteristics are persisted).
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/settings/persistent_value_registry.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/settings/persistent_value_store.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
# bt_service_cpp.h trips -Wcomment (known non-blocking warning, see fw/CLAUDE.md)
target_compile_options(app PRIVATE -Wno-error=comment)
