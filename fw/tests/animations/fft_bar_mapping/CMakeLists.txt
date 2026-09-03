# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fft_bar_mapping_tests)

# fft_bar_mapping.h is header-only and kernel/BT-free by design (it needs only
# the bucket layout from audio_dsp.h and <math.h>), so the suite compiles it
# straight into the test app - no animation, sound or BT plumbing.
target_sources(app PRIVATE src/main.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src ${CMAKE_CURRENT_LIST_DIR}/../../../include)
