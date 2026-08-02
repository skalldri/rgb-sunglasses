cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_dsp_replay)

# Compiles the REAL firmware DSP (identical translation unit, identical SDK
# CMSIS-DSP) into a native_sim executable that can replay WAV recordings made
# on-device — see fw/docs/beat-detection-debugging.md for the workflow.
target_sources(app PRIVATE src/main.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/audio_dsp.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound)
