zephyr_library_named(rgbx_wasm3)

zephyr_library_sources(
  source/m3_bind.c
  source/m3_code.c
  source/m3_compile.c
  source/m3_core.c
  source/m3_env.c
  source/m3_exec.c
  source/m3_function.c
  source/m3_info.c
  source/m3_module.c
  source/m3_parse.c
  source/m3_validate.c
)

zephyr_library_include_directories(source)

# This MVP runs one small, memoryless module. Keep Wasm3's arena and compiler
# bounds explicit so enabling the experiment cannot consume the system heap or
# silently inherit desktop-sized defaults.
zephyr_library_compile_definitions(
  d_m3CodePageAlignSize=1024
  d_m3MaxFunctionStackHeight=128
  d_m3CascadedOpcodes=0
  d_m3VerboseErrorMessages=0
  d_m3FixedHeap=${CONFIG_APP_WASM3_MVP_HEAP_SIZE}
  d_m3HasFloat=0
  d_m3MaxLinearMemoryPages=1
  d_m3RecordBacktraces=0
)

# Keep upstream-only warning workarounds scoped to the vendored interpreter.
# The unused locals feed logging expressions compiled out by the configuration
# above. v_read_blocktype() writes its output on every success path and the
# caller returns on every error path, but GCC 12 cannot prove that interprocedurally.
zephyr_library_compile_options(
  -Wno-sign-compare
  -Wno-unused-variable
  -Wno-maybe-uninitialized
)
