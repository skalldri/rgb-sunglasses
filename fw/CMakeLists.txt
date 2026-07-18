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
)

# The TPS25750 patch blob: LZ4-compressed (decompressed into RAM at first use) or
# uncompressed rodata, selected per-board — see CONFIG_TPS25750_COMPRESSED_PATCH in
# prj.conf (proto0: uncompressed, saves 14.3KB RAM) and the DK board .conf (compressed,
# saves ~4.8KB of its nearly-full FLASH slot).
if(CONFIG_TPS25750_COMPRESSED_PATCH)
  target_sources(app PRIVATE misc/tps25750/tps25750-config-compressed.c)
elseif(CONFIG_TPS25750_INTERNAL_PATCH)
  if(CONFIG_APP_TPS25750_CONFIG_9V)
    # 9V/5V sink-caps variant — same symbol/size as the stock bundle, see
    # the APP_TPS25750_CONFIG_9V Kconfig help.
    target_sources(app PRIVATE misc/tps25750/tps25750-config-9v.c)
  else()
    target_sources(app PRIVATE misc/tps25750/tps25750-config.c)
  endif()
endif()

target_sources_ifdef(CONFIG_AUDIO app PRIVATE src/sound/sound.cpp)
target_sources_ifdef(CONFIG_AUDIO app PRIVATE src/sound/audio_dsp.cpp)
target_sources_ifdef(CONFIG_AUDIO app PRIVATE src/sound/audio_config.cpp)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/storage/storage.cpp)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/storage/glim_decoder.cpp)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/storage/glim_registry.cpp)
target_sources_ifdef(CONFIG_APP_SETTINGS_PARTITION_ERASE app PRIVATE src/storage/appcfg_erase.cpp)
target_sources_ifdef(CONFIG_APP_FACTORY_RESET app PRIVATE src/factory_reset/factory_reset.cpp)
target_sources_ifdef(CONFIG_APP_FACTORY_RESET app PRIVATE src/factory_reset/factory_reset_core.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_host.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_manifest.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_param_persistence.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_registry.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_animation_proxy.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_bt.cpp)

# BT-free decision core for the connection-parameter governor (issue #188);
# the wiring lives in src/bluetooth.cpp behind the same Kconfig symbol.
target_sources_ifdef(CONFIG_APP_BT_CONN_PARAM_GOVERNOR app PRIVATE
                     src/bluetooth/conn_param_governor_core.cpp)
target_sources_ifdef(CONFIG_APP_EXTENSION_HOST app PRIVATE src/extensions/extension_metadata_blob.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/animations/my_eyes_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/animations/rainbow_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG app PRIVATE src/animations/zigzag_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_BEAT    app PRIVATE src/animations/beat_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_FFT_BARS app PRIVATE src/animations/fft_bars_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_GLIM_PLAYER app PRIVATE src/animations/glim_player_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MATRIX_CODE app PRIVATE src/animations/matrix_code_animation.cpp)
target_sources_ifdef(CONFIG_IMU app PRIVATE src/imu/imu.cpp)
target_sources_ifdef(CONFIG_ANIMATION_TILT app PRIVATE src/animations/tilt_animation.cpp)

# The extension host also binds these adapter sources (rgbx_inputs audio/IMU
# snapshots), so they must compile whenever it is enabled with the matching
# subsystem — not only when a built-in animation wants them.
if(CONFIG_ANIMATION_BEAT OR CONFIG_ANIMATION_FFT_BARS OR (CONFIG_APP_EXTENSION_HOST AND CONFIG_AUDIO))
  target_sources(app PRIVATE src/sound/animation_adapters/audio_animations_sound.cpp)
endif()

if(CONFIG_ANIMATION_TILT OR (CONFIG_APP_EXTENSION_HOST AND CONFIG_IMU))
  target_sources(app PRIVATE src/imu/animation_adapters/imu_animations_imu.cpp)
endif()

target_sources(app PRIVATE src/bluetooth/animation_adapters/text_animation_bt.cpp)

target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/bluetooth/animation_adapters/my_eyes_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/bluetooth/animation_adapters/rainbow_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG  app PRIVATE src/bluetooth/animation_adapters/zigzag_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_BEAT    app PRIVATE src/bluetooth/animation_adapters/beat_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_FFT_BARS app PRIVATE src/bluetooth/animation_adapters/fft_bars_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_GLIM_PLAYER app PRIVATE src/bluetooth/animation_adapters/glim_player_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_MATRIX_CODE app PRIVATE src/bluetooth/animation_adapters/matrix_code_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_TILT app PRIVATE src/bluetooth/animation_adapters/tilt_animation_bt.cpp)

target_sources_ifdef(CONFIG_STATUS_LED app PRIVATE src/status_led/status_led.cpp)

target_sources_ifdef(CONFIG_NETCORE_VERSION_IPC app PRIVATE src/netcore_version.c)
target_sources_ifdef(CONFIG_APP_MCUBOOT_INFO_SERVICE app PRIVATE src/mcuboot_info.cpp)
target_sources_ifdef(CONFIG_APP_MCUBOOT_UPDATER app PRIVATE
    src/mcuboot_updater.cpp
    src/bluetooth/mcuboot_updater_service.cpp
)
target_sources_ifdef(CONFIG_APP_CHARGER_POLICY app PRIVATE src/power/charger_policy.cpp)
target_sources_ifdef(CONFIG_BQ25792 app PRIVATE src/power/comm_health.cpp)
target_sources_ifdef(CONFIG_APP_BATTERY_MONITOR app PRIVATE src/bluetooth/battery_service.cpp)
target_sources_ifdef(CONFIG_APP_POWER_DEBUG_SERVICE app PRIVATE src/bluetooth/power_debug_service.cpp)

target_sources_ifdef(CONFIG_APP_COREDUMP_MANAGER app PRIVATE
    src/debug/coredump_manager.cpp
    src/debug/coredump_manager_core.cpp
)
target_sources_ifdef(CONFIG_APP_CRASH_TEST_COMMANDS app PRIVATE src/debug/crash_shell.cpp)

target_include_directories(
  app 
  PRIVATE
    misc/
    src/
)

# Needs to be accessible to drivers so that the TPS25750 can access the patch file
zephyr_include_directories(misc)

# nRF5340 Anomaly 211: device misses USB RESUME when host restarts SOF packets
# without a formal K-state RESUME signal, causing Windows to escalate to a USB
# Reset. The workaround (polling-based SOF detection during suspend) is already
# implemented in udc_nrf.c but disabled by default behind an #ifndef guard.
zephyr_compile_definitions(NRF_USBD_COMMON_USE_WORKAROUND_FOR_ANOMALY_211=1)

# NORDIC SDK APP END
