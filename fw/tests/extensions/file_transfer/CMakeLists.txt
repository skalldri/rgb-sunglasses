# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(extension_file_transfer_tests)

target_sources(app PRIVATE src/main.cpp)
# extension_registry.cpp is deliberately NOT linked: kDirectory is an inline
# constexpr in its header, so the predicate needs no definition from it, and
# linking it would drag fs_* (and therefore a filesystem stack) into a test
# that only exercises string handling.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/extensions/extension_file_transfer.cpp)
# The predicate itself (shared with the FILE_MGMT custom group) lives in its
# own pure translation unit — see extension_path.h.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/extensions/extension_path.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
