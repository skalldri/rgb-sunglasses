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
    src/usb/usb_init.c
    src/led_controller.cpp
    src/pattern_controller.cpp
    src/buttons.cpp
    src/buttons/animation_adapters/button_animation_source.cpp
    src/power.cpp
    src/bluetooth.cpp
    src/core_config.cpp
    # Always compiled, like every other source in this list - CONFIG_APP_PERSIST_BT_CONFIG
    # gates their *contents* (see the #if in each .cpp), not whether they're built, since
    # other unconditionally-compiled callers (e.g. pattern_controller.cpp) reference their
    # public API regardless of the Kconfig setting.
    src/settings/persistent_value_registry.cpp
    src/settings/persistent_value_store.cpp
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
target_sources_ifdef(CONFIG_AUDIO app PRIVATE src/sound/audio_dsp.cpp)
target_sources_ifdef(CONFIG_AUDIO app PRIVATE src/sound/audio_config.cpp)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/storage/storage.cpp)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/storage/glim_decoder.cpp)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/storage/glim_registry.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/animations/my_eyes_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/animations/rainbow_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG app PRIVATE src/animations/zigzag_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_BEAT    app PRIVATE src/animations/beat_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_FFT_BARS app PRIVATE src/animations/fft_bars_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_GLIM_PLAYER app PRIVATE src/animations/glim_player_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MATRIX_CODE app PRIVATE src/animations/matrix_code_animation.cpp)

if(CONFIG_ANIMATION_BEAT OR CONFIG_ANIMATION_FFT_BARS)
  target_sources(app PRIVATE src/sound/animation_adapters/audio_animations_sound.cpp)
endif()

target_sources(app PRIVATE src/bluetooth/animation_adapters/text_animation_bt.cpp)

target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/bluetooth/animation_adapters/my_eyes_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/bluetooth/animation_adapters/rainbow_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG  app PRIVATE src/bluetooth/animation_adapters/zigzag_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_BEAT    app PRIVATE src/bluetooth/animation_adapters/beat_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_FFT_BARS app PRIVATE src/bluetooth/animation_adapters/fft_bars_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_GLIM_PLAYER app PRIVATE src/bluetooth/animation_adapters/glim_player_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MATRIX_CODE app PRIVATE src/bluetooth/animation_adapters/matrix_code_animation_bt.cpp)

target_sources_ifdef(CONFIG_STATUS_LED app PRIVATE src/status_led/status_led.cpp)

target_sources_ifdef(CONFIG_NETCORE_VERSION_IPC app PRIVATE src/netcore_version.c)
target_sources_ifdef(CONFIG_RETENTION_BOOTLOADER_INFO app PRIVATE src/mcuboot_info.cpp)

target_include_directories(
  app 
  PRIVATE
    misc/
    src/
)

# Needs to be accessible to drivers so that the TPS25750 can access the patch file
zephyr_include_directories(misc)

# NORDIC SDK APP END
