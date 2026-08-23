# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wasm_mvp_runtime_tests)

set(FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

# These are ordinary Kconfig values in the firmware application. The focused
# QEMU test compiles the same runtime source without importing the whole
# product Kconfig tree, so keep its bounded test profile explicit here.
set(CONFIG_APP_WASM3_MVP_HEAP_SIZE 16384)
target_compile_definitions(app PRIVATE
  CONFIG_APP_WASM3_MVP_HEAP_SIZE=16384
  CONFIG_APP_WASM3_MVP=1
  CONFIG_APP_WASM3_V2_PROTOTYPE=1
  CONFIG_APP_WASM3_V2_ADMISSION_CPU_BUDGET_MS=250
  CONFIG_APP_WASM3_MVP_VALUE_STACK_SIZE=1024
  CONFIG_APP_WASM3_MVP_MODULE_MAX_SIZE=2048
  CONFIG_APP_WASM3_MVP_THREAD_PRIORITY=9
  CONFIG_APP_WASM3_MVP_THREAD_STACK_SIZE=4096
  CONFIG_APP_WASM3_MVP_CPU_BUDGET_MS=50
  CONFIG_APP_WASM3_MVP_WALL_BACKSTOP_MS=500
  CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY=4
)

add_subdirectory(${FW_ROOT}/third_party/wasm3 wasm3)
target_link_libraries(app PRIVATE rgbx_wasm3)

target_sources(app PRIVATE
  src/main.cpp
  ${FW_ROOT}/src/extensions/wasm_mvp_runtime.cpp
  ${FW_ROOT}/src/extensions/sandbox_fatal_handler.cpp
)

target_include_directories(app PRIVATE
  ${FW_ROOT}/src
  ${FW_ROOT}/include
  ${FW_ROOT}/third_party/wasm3
  ${FW_ROOT}/third_party/wasm3/source
)
