# Included by sysbuild_add_subdirectory's while loop for the fw image, after all images
# are registered but before ExternalZephyrProject_Cmake runs for any image.
# This is the correct hook to append to IMAGE_CONF_SCRIPT.
if(TARGET mcuboot)
  set_property(TARGET mcuboot APPEND PROPERTY IMAGE_CONF_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_version_conf.cmake")
endif()
