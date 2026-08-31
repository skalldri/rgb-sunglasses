cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_telemetry_control_tests)

# audio_telemetry_control.h is header-only and dependency-free (see the header's comment for
# why the decode was split out of the service at all), so the header under test IS the unit —
# no BT stack, no GATT server, nothing to link.
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound)
