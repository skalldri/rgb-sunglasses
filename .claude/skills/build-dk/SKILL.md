---
description: Build the DK (rgb_sunglasses_dk) firmware — kept in a separate build dir to avoid costly board-switch rebuilds
allowed-tools: Bash, Read
---

Build the DK firmware. Build dir: `fw/build-dk`. This is a SEPARATE directory from the proto0 build (`fw/build`) — never mix the two, as switching board types in the same dir triggers a full pristine rebuild.

```bash
west build \
  --build-dir /workspaces/rgb-sunglasses/fw/build-dk \
  /workspaces/rgb-sunglasses/fw \
  --board rgb_sunglasses_dk/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="/workspaces/rgb-sunglasses/fw"
```

## Steps

1. Run the build command above.
2. If it **fails**:
   - Do NOT re-run the build immediately. Read the error from the build output first.
   - If a compilation error: read the relevant source files, fix the code, then rebuild.
   - If a linker overflow error (`Image size ... exceeds requested size`): the DK appcore has a tight flash budget. Features gated on `CONFIG_APP_PERSIST_BT_CONFIG`, `CONFIG_APP_BT_METADATA_CHARACTERISTIC`, or board-specific guards must be disabled on DK (`n` in `boards/others/rgb_sunglasses_dk/rgb_sunglasses_dk_nrf5340_cpuapp.conf`). Do NOT add new code to the DK build without checking flash usage first.
   - Only rebuild after a concrete fix is applied.
3. If it **succeeds**: report the appcore FLASH and RAM usage percentages. The DK slot is tight (~222 KB). Warn if FLASH > 90%.
