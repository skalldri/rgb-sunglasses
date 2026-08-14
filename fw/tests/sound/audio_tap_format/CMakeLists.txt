cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_tap_format_tests)

# audio_tap_format.h is header-only and Zephyr-free by design (so the native_sim
# replay app can include it unchanged), so there is nothing to link — the header
# under test IS the unit.
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound)
