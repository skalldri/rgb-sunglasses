# ROM-reduction history (issue #79)

Historical measurements, kept for the reasoning behind today's configuration. **Not current
numbers** — rebuild and read the linker map for those (parent skill).

## First pass — making `CONFIG_USERSPACE` fit

`CONFIG_USERSPACE=y` alone costs ~105-246 KB of FLASH (`.claude/rules/fw-userspace.md`). Fitting it
required:

- `CONFIG_DUMP_DEVICE_REGISTERS=n` (~94 KB; rationale next to it in
  `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`)
- `CONFIG_FLASH_SIMULATOR_STATS=n` in `fw/prj.conf` (~4 KB)
- a `tuple_cat` collapse in `fw/src/bluetooth/bt_service_cpp.h` (see that file's comment)

## Second pass (issue #79 follow-up) — 94.6 % → 64.6 % appcore FLASH

Measured at 624,492 B of the then-966,144 B `app` slot. (The slot is 900,608 B since the issue #80
coredump partition was carved out of internal flash —
`fw/pm_static_rgb_sunglasses_proto0_nrf5340_cpuapp.yml`.) Deltas:

- `CONFIG_SIZE_OPTIMIZATIONS=y` replacing `CONFIG_DEBUG_OPTIMIZATIONS=y` (proto0 board conf):
  **~255 KB**. The entire image had been compiling `-Og`. Flip back temporarily for deep GDB
  sessions if needed.
- Shell pruning (`CONFIG_SENSOR_SHELL=n`, `CONFIG_FLASH_SHELL=n`, `CONFIG_DEVMEM_SHELL=n`): **~29 KB**
  FLASH + ~21 KB RAM.
- Float printf removal (`CONFIG_CBPRINTF_FP_SUPPORT=n`, `CONFIG_PICOLIBC_IO_FLOAT=n`, after converting
  `sound.cpp` to integer prints): **~4.9 KB**.
- Deliberately kept: LLEXT (+shell/EDK, ~20-30 KB — groundwork for loadable extensions),
  DEBUG_COREDUMP (feature planned, since shipped as issue #80), CMSIS-DSP (all four sub-options
  genuinely used by `audio_dsp.cpp`: rfft_fast/cmplx_mag_squared/mean/std/hanning).
- Biggest remaining single item: `bt_service_cpp.h` template instantiations, **~70 KB** of `fw/src`'s
  ~152 KB (per-service `BtGattServer<...>` constructors + `tupleToArray` expansions). Recovering it
  means building the `bt_gatt_attr` tables at runtime instead of per-service templates — a separate,
  riskier refactor (issue #84).
