#
# Copyright (c) 2023 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(NONE)

add_subdirectory(drivers)
add_subdirectory(include)

# NORDIC SDK APP START
target_sources(
  app 
  PRIVATE
    src/main.c
    src/led_controller.cpp
    src/pattern_controller.cpp
    src/buttons.cpp
    src/power.cpp
    src/bluetooth.cpp
    src/core_config.cpp
    src/animations/bt_animations.cpp
    src/animations/null_animation.cpp
    src/animations/text_animation.cpp
    src/animations/animation_registry.cpp
    src/animations/animation_registry_defaults.cpp
    src/animations/animation_base.cpp
    src/fonts/FontAtlas.cpp
    src/fonts/FontShell.cpp
    misc/tps25750/tps25750-config-compressed.c
)

target_sources_ifdef(CONFIG_AUDIO app PRIVATE src/sound/sound.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/animations/my_eyes_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/animations/rainbow_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG app PRIVATE src/animations/zigzag_animation.cpp)

target_sources(app PRIVATE src/bluetooth/animation_adapters/text_animation_bt.cpp)

target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/bluetooth/animation_adapters/my_eyes_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/bluetooth/animation_adapters/rainbow_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG  app PRIVATE src/bluetooth/animation_adapters/zigzag_animation_bt.cpp)

target_include_directories(
  app 
  PRIVATE
    misc/
    src/
)

# Needs to be accessible to drivers so that the TPS25750 can access the patch file
zephyr_include_directories(misc)

# NORDIC SDK APP END
