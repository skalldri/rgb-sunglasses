---
name: build-proto0
description: Incrementally build the proto0 (rgb_sunglasses_proto0) firmware — the primary day-to-day build target
allowed-tools: Bash, Read
---

Build the proto0 firmware. Build dir: `fw/build`. Never use `--pristine` unless explicitly asked.

```bash
west build \
  --build-dir /workspaces/rgb-sunglasses/fw/build \
  /workspaces/rgb-sunglasses/fw \
  --board rgb_sunglasses_proto0/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="/workspaces/rgb-sunglasses/fw"
```

## Steps

1. Run the build command above.
2. If it **fails**:
   - Do NOT re-run the build immediately. Read the error from the build output first.
   - If a compilation error: read the relevant source files, fix the code, then rebuild.
   - If a CMake/Kconfig error: check `fw/build/fw/CMakeFiles/CMakeError.log`.
   - Only rebuild after a concrete fix is applied.
3. If it **succeeds**: report the appcore FLASH and RAM usage percentages from the build output. Warn if FLASH > 95%.
