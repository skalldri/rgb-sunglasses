cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_param_blob_tests)

# audio_param_blob.h and audio_param_table.h are header-only and BT-free, so the headers under
# test ARE the unit — the same discipline as the codec suite. The 0x2906 descriptor bytes are
# built by the table header too, which is what lets this suite check the descriptor and the
# blob agree without linking the GATT server.
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound)
