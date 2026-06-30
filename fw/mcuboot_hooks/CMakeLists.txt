if(CONFIG_BOOT_GO_HOOKS)
  zephyr_library()
  zephyr_library_sources(fprotect_hook.c)
  # Build fprotect_spu.c directly: CONFIG_FPROTECT=n prevents the nrf module from
  # building it, but fprotect_hook.c still needs fprotect_area() for the normal-boot
  # path. fprotect_spu.c has no CONFIG_FPROTECT guards internally so it compiles fine.
  zephyr_library_sources(${ZEPHYR_NRF_MODULE_DIR}/lib/fprotect/fprotect_spu.c)
  zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
endif()
