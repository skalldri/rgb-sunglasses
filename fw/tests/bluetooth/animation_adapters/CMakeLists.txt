# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(animation_adapters_tests)

target_sources(app PRIVATE src/main.cpp)

# Units under test: every src/bluetooth/animation_adapters/*.cpp EXCEPT
# fft_bars_animation_bt.cpp (see the comment in src/main.cpp - it transitively
# requires src/sound/sound.cpp, which references real proto0 devicetree nodes
# (vm3011/pdm0) that don't exist on native_sim; that gap is issue #83, out of
# scope here and explicitly deferred).
set(ADAPTER_SRCS
    zigzag_animation_bt
    tilt_animation_bt
    beat_animation_bt
    pulse_animation_bt
    rainbow_animation_bt
    matrix_code_animation_bt
    my_eyes_animation_bt
    text_animation_bt
    glim_player_animation_bt
)
foreach(name ${ADAPTER_SRCS})
    target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/bluetooth/animation_adapters/${name}.cpp)
endforeach()

# Each adapter's BT-free animation class (needed for the vtable/getInstance()
# body every *_bind_default_*dependencies() function calls into).
set(ANIMATION_SRCS
    zigzag_animation
    tilt_animation
    beat_animation
    pulse_animation
    rainbow_animation
    matrix_code_animation
    my_eyes_animation
    text_animation
    glim_player_animation
)
foreach(name ${ANIMATION_SRCS})
    target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/animations/${name}.cpp)
endforeach()

target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/animations/animation_registry.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/animations/animation_base.cpp)

# text/my_eyes animations render via FontAtlas/FontShell; glim_player needs
# the real GLIM decoder/registry (all three already proven native_sim-safe by
# their own tests/animations/*_animation_di suites - same recipe, reused here).
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/fonts/FontAtlas.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/fonts/FontShell.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/storage/glim_decoder.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/storage/glim_registry.cpp)

# BtGattPersistentCharacteristic is exercised for real (CONFIG_APP_PERSIST_BT_CONFIG=y
# below), same as tests/bluetooth/battery_service.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/settings/persistent_value_registry.cpp)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src/settings/persistent_value_store.cpp)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../src)

# bt_service_cpp.h trips -Wcomment (known non-blocking warning, see fw/CLAUDE.md)
target_compile_options(app PRIVATE -Wno-error=comment)
