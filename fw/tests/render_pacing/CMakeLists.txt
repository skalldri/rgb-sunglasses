# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(render_pacing_tests)

# render_pacing.h is header-only and kernel-free by design (the pure divider
# arithmetic of the issue-#379 frame-consumed handshake), so the suite compiles
# it straight into the test app — pattern_controller.cpp itself needs the whole
# render stack and cannot build here.
target_sources(app PRIVATE src/main.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
