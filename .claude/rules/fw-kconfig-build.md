---
paths:
  - "fw/Kconfig"
  - "fw/Kconfig.*"
  - "fw/**/Kconfig"
  - "fw/prj.conf"
  - "fw/boards/**/*.conf"
  - "fw/CMakeLists.txt"
---

# Kconfig, CMake gates and build configuration

Loads when you read Kconfig, `.conf` fragments or `fw/CMakeLists.txt`. Per-image (MCUboot, b0n,
ipc_radio) overlays are `.claude/rules/fw-sysbuild-mcuboot.md`; size work is `/rom-ram-budget`.

## An incremental build IGNORES a changed Kconfig `default`

Zephyr loads the existing `fw/build/fw/zephyr/.config` as the **base** for each configure pass, so
editing a `default` in `fw/Kconfig` (or a driver `Kconfig`) and rebuilding incrementally silently
keeps the **old** value. The build succeeds, so nothing warns you — verified 2026-08-01 while
retuning thread priorities for issue #267: `default 3` → `default 2` rebuilt clean and
`autoconf.h` still read `3`, and the flashed board ran the old priority.

This only affects **defaults**. An explicit `CONFIG_FOO=...` in `prj.conf` or a board `.conf`
*does* override on an incremental build — that asymmetry is what makes the failure so easy to miss.
Either of these fixes it:

```bash
rm fw/build/fw/zephyr/.config      # cheaper than --pristine; forces one Kconfig regeneration
```

or set the value explicitly in `fw/prj.conf` instead of relying on the default (also the right way
to build a throwaway A/B variant without touching committed defaults). **Always confirm the value
actually landed before flashing:**

```bash
grep CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY fw/build/fw/zephyr/include/generated/zephyr/autoconf.h
```

## Compile gates and feature symbols

- Each built-in animation is gated by a `CONFIG_ANIMATION_<NAME>` symbol in `fw/Kconfig` (e.g.
  `CONFIG_ANIMATION_RAINBOW=y`); Text is always compiled. Audio is gated on `CONFIG_AUDIO`.
- App modules compile via `target_sources_ifdef(CONFIG_<MODULE> app PRIVATE ...)` lines in
  `fw/CMakeLists.txt` — that CMake line is the compile gate; in-source `#if DT_HAS_ALIAS(...)`
  guards (e.g. `led_strip_2` in `fw/src/status_led/status_led.cpp`) are secondary, never the gate.
  When adding a tunable for an existing module, check that module's `target_sources_ifdef` line
  first and add `depends on <MODULE>` to the new symbol, matching every other `APP_*` int in
  `fw/Kconfig` (e.g. `APP_EXT_TICK_CPU_BUDGET_MS` depends on `APP_EXTENSION_HOST`).
- **Don't reuse a Kconfig symbol from one subsystem to configure unrelated code in another, even if
  the value/semantics happen to line up.** A BT-free module's debounce tunable gets its own
  `CONFIG_APP_*` symbol, not `CONFIG_BT_SETTINGS_DELAYED_STORE_MS` just because the timing matches —
  that creates a hidden cross-subsystem dependency and works against the project's push to decouple
  BT from non-BT code.
- Every thread priority and stack size is a Kconfig symbol (issue #269) — the `Thread priorities and
  stack sizes` menu in `fw/Kconfig`, plus `IMU_THREAD_*` / `APP_EXT_HOST_*` /
  `APP_*_WORKQ_STACK_SIZE` next to their modules. The ordering invariants are `BUILD_ASSERT`s next to
  the threads they constrain (`fw/docs/threading.md`).

## Memory-saving configuration

- `fw/prj.conf` carries the memory-saving flags (`CONFIG_ASSERT=n`, `CONFIG_CBPRINTF_FP_SUPPORT=n`,
  `CONFIG_PICOLIBC_IO_FLOAT=n`, `CONFIG_FLASH_SIMULATOR_STATS=n`); `CONFIG_SIZE_OPTIMIZATIONS=y`
  and `CONFIG_DUMP_DEVICE_REGISTERS=n` live in the proto0 **board** conf
  (`fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`, with the size rationale next to them).
- Disabling float printf is why `%f` prints literally — `fw/CLAUDE.md` "Coding rules" and
  `.claude/rules/fw-logging.md`.
- Measured history of these choices: `.claude/skills/rom-ram-budget/references/rom-pass-history.md`.
