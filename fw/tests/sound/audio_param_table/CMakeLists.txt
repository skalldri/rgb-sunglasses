cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_param_table_tests)

# audio_param_table.h is header-only, constexpr, Zephyr-free and free of project
# headers by design (so the native_sim replay app can include it unchanged), so
# there is nothing to link — the header under test IS the unit.
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound)
