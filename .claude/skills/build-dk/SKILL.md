---
name: build-dk
description: Build the DK (rgb_sunglasses_dk) firmware — kept in a separate build dir to avoid costly board-switch rebuilds
allowed-tools: Bash, Read
---

Build the DK firmware. Build dir: `fw/build-dk`. This is a SEPARATE directory from the proto0 build (`fw/build`) — never mix the two, as switching board types in the same dir triggers a full pristine rebuild.

**Always capture the entire build output to a temp file** (the `tee` below) — a sysbuild run links 4
images and prints 4 separate memory tables, so anything you need later (the appcore table, an error
further up) can be pulled from the file with Read/Grep instead of re-running the whole build.

```bash
west build \
  --build-dir fw/build-dk \
  fw \
  --board rgb_sunglasses_dk/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="$(pwd)/fw" 2>&1 | tee "$SCRATCHPAD_OR_TMP/build-dk.log" | tail -30
```

(`$SCRATCHPAD_OR_TMP` = your session scratchpad dir, or `/tmp` outside a Claude session.)

## Steps

0. **First-time build check**: if `[ ! -f fw/build-dk/fw/CMakeCache.txt ]`, no configured build exists yet.
   Run the exact same command — it configures from scratch. This is **very slow (tens of minutes)**: a
   full sysbuild of netcore + MCUboot + app. Do not interrupt or retry it; let it finish. On all
   subsequent runs the same command builds incrementally — never add `--pristine` unless the user asks
   or the board/sysbuild config changed incompatibly.
1. Run the build command above.
2. If it **fails**:
   - Do NOT re-run the build immediately. Read the error from the build output first.
   - If a compilation error: read the relevant source files, fix the code, then rebuild.
   - If a linker/image overflow error (`Image size ... exceeds requested size` or
     `region ... overflowed`): this is **expected system behavior, not a mystery** — DK appcore FLASH
     sits historically at 92–94% (as of 2026-07 — re-verify from build output; `fw/CLAUDE.md` quotes
     "~95% used" for the same budget — same fact, coarser rounding — the build output is the
     authority), so a change that fits proto0 fine can overflow the DK. That is exactly why /submit-pr requires both boards to build.
     The sanctioned fix for a DK-only overflow is to disable the new feature on DK: set its Kconfig
     symbol to `n` in `fw/boards/rgb_sunglasses_dk_nrf5340_cpuapp.conf` (precedent in that file:
     `CONFIG_APP_PERSIST_BT_CONFIG=n`, `CONFIG_APP_BT_METADATA_CHARACTERISTIC=n`,
     `CONFIG_APP_MCUBOOT_INFO_SERVICE=n` — DK is legacy and gets no new features per `fw/CLAUDE.md`).
     The `=n` override only reclaims flash if the feature's code compiles out cleanly when disabled —
     wrap call sites in `IS_ENABLED(CONFIG_...)` / `if constexpr (IS_ENABLED(...))` or gate the sources
     with `target_sources_ifdef` in `fw/CMakeLists.txt` (working precedent:
     `CONFIG_APP_PERSIST_BT_CONFIG`, documented in `fw/CLAUDE.md`).
     **Path warning**: board Kconfig fragments live FLAT under `fw/boards/`. The directory
     `fw/boards/others/rgb_sunglasses_dk/` holds only the board *definition* (dts, defconfig,
     board.yml) — a `.conf` created there is **silently ignored**. For deeper size analysis
     (what grew, where it lives), use /rom-ram-budget.
   - Only rebuild after a concrete fix is applied.
3. If it **succeeds**: report the appcore FLASH and RAM usage percentages from the build output. The
   DK flash budget is tight — 92–94% FLASH is the historical norm (as of 2026-07), so a high number
   alone is not alarming; call it out only if it moved noticeably toward 100% relative to `main`, and
   route any deeper size claim through /rom-ram-budget.
