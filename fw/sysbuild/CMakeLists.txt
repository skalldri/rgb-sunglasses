set(SB_APPLICATION_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/../conf/\${NORMALIZED_BOARD_TARGET}")

# Make the mcuboot_hooks Zephyr module available to every image build.
# The module's CMakeLists.txt is guarded by if(CONFIG_BOOT_GO_HOOKS), so it
# only compiles fprotect_hook.c in the MCUboot image and is a no-op elsewhere.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_LIST_DIR}/../mcuboot_hooks")
set(ZEPHYR_EXTRA_MODULES "${ZEPHYR_EXTRA_MODULES}" CACHE STRING
    "Extra Zephyr modules (mcuboot_hooks)" FORCE)

# Derive the version string from the project VERSION file and inject it as a
# sysbuild Kconfig override via SB_EXTRA_CONF_FILE.  Without this,
# b0_mcuboot_signing.cmake hardcodes "0.0.0+0" for the ipc_radio MCUBoot header
# (SB_CONFIG_SECURE_BOOT_MCUBOOT_VERSION), so the netcore and appcore images end
# up with different versions even though they share the same VERSION file.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" _ver_lines)
foreach(_line ${_ver_lines})
  if(_line MATCHES "VERSION_MAJOR *= *([0-9]+)")
    set(_ver_major ${CMAKE_MATCH_1})
  elseif(_line MATCHES "VERSION_MINOR *= *([0-9]+)")
    set(_ver_minor ${CMAKE_MATCH_1})
  elseif(_line MATCHES "PATCHLEVEL *= *([0-9]+)")
    set(_ver_patch ${CMAKE_MATCH_1})
  elseif(_line MATCHES "VERSION_TWEAK *= *([0-9]+)")
    set(_ver_tweak ${CMAKE_MATCH_1})
  endif()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/netcore_version_override.conf"
     "SB_CONFIG_SECURE_BOOT_MCUBOOT_VERSION=\"${_ver_major}.${_ver_minor}.${_ver_patch}+${_ver_tweak}\"\n")
set(SB_EXTRA_CONF_FILE "${CMAKE_BINARY_DIR}/netcore_version_override.conf")

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)