# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(extension_bt_tests)

target_sources(app PRIVATE src/main.cpp)

# Unit under test.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/extensions/extension_bt.cpp)

# Real, Zephyr-free support code extension_bt.cpp depends on - no reason to
# fake what's already pure logic (extension_metadata_blob, animation_registry,
# animation_base). extension_host.cpp and pattern_controller.cpp are NOT
# linked here: they pull in llext/FAT/LED-controller/thread machinery this
# suite has no business exercising, so their extension_bt.cpp-visible surface
# is faked in src/main.cpp instead (extension_host namespace functions,
# pattern_controller_change_to_animation/get_current_animation).
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/extensions/extension_metadata_blob.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/animations/animation_registry.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/animations/animation_base.cpp)

# rgbx/rgbx_api.h (extension ABI header)
add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_BINARY_DIR}/app_include
)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)

# bt_service_cpp.h trips -Wcomment (known non-blocking warning, see fw/CLAUDE.md)
target_compile_options(app PRIVATE -Wno-error=comment)
