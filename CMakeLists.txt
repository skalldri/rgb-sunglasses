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
    src/animations/zigzag_animation.cpp
    src/animations/text_animation.cpp
    src/animations/rainbow_animation.cpp
    src/fonts/FontAtlas.cpp
    src/fonts/FontShell.cpp
    src/sound/sound.cpp
    misc/tps25750/tps25750-config.c
    # misc/tps25750/tps25750-full-config.c
)

target_include_directories(
  app 
  PRIVATE
    misc/
    src/
)

# NORDIC SDK APP END
