---
name: build-proto0
description: Incrementally build the proto0 (rgb_sunglasses_proto0) firmware — the primary day-to-day build target
allowed-tools: Bash, Read
---

Build the proto0 firmware. Build dir: `fw/build`. Never use `--pristine` unless explicitly asked.

```bash
west build \
  --build-dir fw/build \
  fw \
  --board rgb_sunglasses_proto0/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="$(pwd)/fw"
```

## Steps

0. **First-time build check**: if `[ ! -f fw/build/fw/CMakeCache.txt ]`, no configured build exists yet.
   Run the exact same command — it configures from scratch. This is **very slow (tens of minutes)**: a
   full sysbuild of netcore + MCUboot + app. Do not interrupt or retry it; let it finish. On all
   subsequent runs the same command builds incrementally — never add `--pristine` unless the user asks
   or the board/sysbuild config changed incompatibly.
1. Run the build command above.
2. If it **fails**:
   - Do NOT re-run the build immediately. Read the error from the build output first.
   - If a compilation error: read the relevant source files, fix the code, then rebuild.
   - If a CMake/Kconfig error: check `fw/build/fw/CMakeFiles/CMakeError.log`, and see the
     build/config-failure table (table 1) in /debug-fw.
   - Only rebuild after a concrete fix is applied.
3. If it **succeeds**:
   - Report the appcore FLASH and RAM usage percentages from the build output. Warn if FLASH > 95%.
     Any size claim beyond those printed percentages goes through /rom-ram-budget
     (`fw/build/fw/zephyr/zephyr.map` is ground truth).
   - If the change added/edited a Kconfig symbol: confirm it landed in
     `fw/build/fw/zephyr/include/generated/zephyr/autoconf.h` (root CLAUDE.md pre-flash rule).
   - This build is a prerequisite for `fw/extensions/build.sh` and `fw/scripts/provision-device.sh`
     (both default to `fw/build`).
