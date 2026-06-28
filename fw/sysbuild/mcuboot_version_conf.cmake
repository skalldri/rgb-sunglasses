# Conf script for the MCUboot image. Reads fw/sysbuild/mcuboot/VERSION and injects
# the version into MCUboot's generated app_version.h via APP_VERSION_CUSTOMIZATION.
# Included by ExternalZephyrProject_Cmake; BINARY_DIR and ZCMAKE_APPLICATION are set
# in the caller's scope and visible here because include() shares scope.

set(_version_file "${CMAKE_CURRENT_LIST_DIR}/mcuboot/VERSION")
if(NOT EXISTS "${_version_file}")
  message(WARNING "MCUboot custom VERSION file not found: ${_version_file}")
  return()
endif()

file(STRINGS "${_version_file}" _ver_lines)
set(_major 0)
set(_minor 0)
set(_patch 0)
set(_tweak 0)
foreach(_line ${_ver_lines})
  if(_line MATCHES "VERSION_MAJOR *= *([0-9]+)")
    set(_major ${CMAKE_MATCH_1})
  elseif(_line MATCHES "VERSION_MINOR *= *([0-9]+)")
    set(_minor ${CMAKE_MATCH_1})
  elseif(_line MATCHES "PATCHLEVEL *= *([0-9]+)")
    set(_patch ${CMAKE_MATCH_1})
  elseif(_line MATCHES "VERSION_TWEAK *= *([0-9]+)")
    set(_tweak ${CMAKE_MATCH_1})
  endif()
endforeach()

# Write the override header into MCUboot's generated include directory before
# MCUboot's own cmake run. The include path already contains this directory.
set(_override_dir "${BINARY_DIR}/zephyr/include/generated/zephyr")
file(MAKE_DIRECTORY "${_override_dir}")
file(WRITE "${_override_dir}/app_version_override.h"
"#undef APP_VERSION_MAJOR
#define APP_VERSION_MAJOR ${_major}
#undef APP_VERSION_MINOR
#define APP_VERSION_MINOR ${_minor}
#undef APP_PATCHLEVEL
#define APP_PATCHLEVEL ${_patch}
#undef APP_TWEAK
#define APP_TWEAK ${_tweak}
")

# Append to the sysbuild cache so that zephyr_get(APP_VERSION_CUSTOMIZATION SYSBUILD LOCAL)
# in zephyr/CMakeLists.txt (MCUboot's cmake) picks up the include directive.
# The "mcuboot_" prefix means this is only seen by the mcuboot image.
get_target_property(_mcuboot_cache_file mcuboot CACHE_FILE)
file(APPEND "${_mcuboot_cache_file}"
  "mcuboot_APP_VERSION_CUSTOMIZATION:STRING=#include<zephyr/app_version_override.h>\n"
)
