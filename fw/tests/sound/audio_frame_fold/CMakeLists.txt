# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_frame_fold_tests)

# audio_frame_fold.h is header-only and kernel-free by design (it only needs the
# audio_analysis_result struct from audio_dsp.h), so the suite compiles it
# straight into the test app - no sound.cpp / msgq plumbing.
target_sources(app PRIVATE src/main.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
