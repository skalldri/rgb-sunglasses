cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_telemetry_accum_tests)

# The accumulator is Zephyr-aware (spinlock + atomic) but BT-free, so it links directly
# with no Bluetooth stack — the same seam that keeps sound.cpp free of BT dependencies.
target_sources(app PRIVATE src/main.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/audio_telemetry.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound)
