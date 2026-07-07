---
name: debug-fw
description: Diagnose firmware problems — build failures, Twister/native_sim test failures, or on-device symptoms (LEDs dark/wrong, animations invisible, shell wedged, I2C errors, implausible sensor readings, boot loops, missing /dev/ttyACM nodes, J-Link cannot connect). Symptom-to-cause playbook; start here BEFORE guessing at fixes or re-running commands.
---

# Firmware debugging router

Match your symptom against the right table, run the **Verify with** command to confirm the
cause, then apply the fix. Do not re-run a failed build/test/flash "to see if it happens
again" before checking a table.

- **Table 1 (below)** — build & config failures. No hardware needed.
- **Table 2 (below)** — Twister / native_sim test failures. No hardware needed.
- **Table 3** — on-device symptoms: [references/device-symptoms.md](references/device-symptoms.md).
  Every row there requires the `board` hardware lock (`/hw-lock`) and the serial MCP tools.
- Device↔app **Bluetooth** symptoms (app hangs, stale values, GATT errors) → `/debug-ble`.
- To re-flash and re-check on hardware after a fix → `/flash-and-verify`.

> **DANGER ZONE — debugging never includes "trying a write".**
> Reads (`power bq status`, `power pd dump`, register dumps) are safe. **Any** register
> write, 4CC task, or patch download to the TPS25750/BQ25792 (or any physical part)
> requires the authoritative datasheet/TRM first — root `CLAUDE.md`, "NEVER write
> unverified commands or data into hardware parts". A hallucinated 4CC write bricked the
> PD controller on 2026-07-05 — that incident is recorded only in that root `CLAUDE.md`
> section, not the tracker; the related I2Cm-bridge race was later root-caused in
> issue #109 / PR #111. Additionally,
> `power boost` writes UICR `VREGHVOUT` — **irreversible** without a mass chip erase
> (`cmd_power_sys_boost` in `fw/src/power.cpp`). Never run it as a debugging experiment.

## Table 1 — build & config failures

Build dirs: proto0 = `fw/build`, DK = `fw/build-dk` (see `/build-proto0`, `/build-dk`).
Substitute the right one below.

| Symptom | Likely cause | Verify with | Fix / pointer |
|---|---|---|---|
| Linker: `region ... overflowed` / `Image size ... exceeds requested size`, usually on the **DK** build | DK appcore FLASH is chronically near-full (historically 92–94% as of 2026-07 — re-verify) ; a proto0-safe change overflows it | Rebuild and read the FLASH % line; ground truth is `fw/build-dk/fw/zephyr/zephyr.map` | `/rom-ram-budget`. DK-only feature gating lives in `fw/boards/rgb_sunglasses_dk_nrf5340_cpuapp.conf` |
| A `CONFIG_*` symbol you set appears to have no effect | Symbol misspelled, dependency unmet, or overridden — web-searched Kconfig names are often wrong for NCS | `grep CONFIG_<NAME> fw/build/fw/zephyr/include/generated/zephyr/autoconf.h` — absent or wrong value = not applied | Find the real symbol in the NCS source under `/root/ncs/v3.1.1/` (root `CLAUDE.md` rule); fix spelling/deps, rebuild, re-grep autoconf.h **before** flashing |
| Edits to a board `.conf` are ignored | Edited the wrong tree: app-level board Kconfig fragments live **flat** at `fw/boards/<board>_nrf5340_cpuapp.conf`; `fw/boards/others/<board>/` is the board *definition* (DTS, defconfig, board.yml), not the app conf | `ls fw/boards/*.conf` — the two real fragments are `rgb_sunglasses_proto0_nrf5340_cpuapp.conf` and `rgb_sunglasses_dk_nrf5340_cpuapp.conf` | Move the settings to the flat `.conf`; rebuild and verify via autoconf.h as above |
| CMake configure/generate error (before compilation starts) | Toolchain/Kconfig/DT problem; the console error is often truncated | Read `fw/build/fw/CMakeFiles/CMakeError.log` (or `fw/build-dk/fw/CMakeFiles/CMakeError.log`) | Fix the first real error in the log; never retry the build unchanged (`/build-proto0` step 2) |
| `error: "/*" within comment [-Werror=comment]` in a **test** build pulling in `bt_service_cpp.h` | Known `-Wcomment` warning in `fw/src/bluetooth/bt_service_cpp.h`; tests build with warnings-as-errors | `grep -rln 'Wno-error=comment' fw/tests` — every suite that includes that header carries the workaround | Add `target_compile_options(app PRIVATE -Wno-error=comment)` to the test's `CMakeLists.txt` (tests only — the main firmware build does not need or use it). Full test scaffolding: `/add-fw-test` |

## Table 2 — Twister / native_sim test failures

Run tests via `/test-fw`. Per-scenario artifacts:
`fw/twister-out/native_sim_native/host/<path-relative-to-fw/tests>/<dotted.scenario>/`.

| Symptom | Likely cause | Verify with | Fix / pointer |
|---|---|---|---|
| Twister reports a suite failed — but which phase? | Build failure vs runtime failure are different files | `build.log` (compile/link) vs `handler.log` (captured ztest stdout) in the scenario dir above | Compile errors → fix source, see the `-Wcomment` row in Table 1. Runtime → next row |
| Need to iterate on one failing case without full Twister runs | Twister overhead hides the actual assert | `cd` into the scenario dir; `./zephyr/zephyr.exe` is a **plain host ELF** — run it directly. `-list` lists cases; `-test="suite::case"` (or `suite::*`) filters; `-seed=<n>` reproduces entropy-dependent runs | Debug with `gdb --args ./zephyr/zephyr.exe -test="suite::case"` — ordinary breakpoints work. `-no-rt` runs simulated time flat-out; `-stop_at=<sim-seconds>` bounds a runaway test |
| `TwisterException` at runtime naming your new suite | `testcase.yaml` scenario name is a plain word — it **must** be dotted `category.name` | `grep -A2 'tests:' fw/tests/<area>/<name>/testcase.yaml` and compare with any in-tree suite (e.g. `fw/tests/animations/animation_registry/testcase.yaml`) | Rename to `<category>.<name>` (rule recorded in `fw/CLAUDE.md`, "Twister testcase.yaml naming") |
| C++ test source fails to compile with standard-library/`std=` errors | Wrong C++23 Kconfig: `CONFIG_STD_CPP23` **does not exist** | `grep CONFIG_STD fw/tests/<suite>/prj.conf` vs a working suite, e.g. `fw/tests/animations/zigzag_animation_di/prj.conf` | `CONFIG_STD_CPP2B=y` (+ `CONFIG_REQUIRES_FULL_LIBCPP=y`, `CONFIG_REQUIRES_FULL_LIBC=y`). Full incantation: `/add-fw-test` |
| Test flaky/failing on timing: events missed, frames dropped, setup races | native_sim time is **simulated**: `k_usleep` rounds up to full 10 ms ticks, so closely-spaced sleeps/interrupts coalesce | Read the worked example comments in `fw/tests/imu/pipeline/src/main.c` (search `coalesce`, `simulated`) | Poll for readiness in suite setup instead of fixed sleeps; space emulated interrupts > 1 tick apart |

## On-device symptoms (Table 3)

Read [references/device-symptoms.md](references/device-symptoms.md). It contains the
symptom table (dark LEDs, wrong animation, I2C errors, implausible readings, freezes,
non-persisting settings, K_USER faults, log spam, GLIM files missing, extension FAULT,
wedged shell/ttyACM, APPROTECT/debug-port lockout + its `nrfutil device recover`
procedure), the read-only serial diagnostic command inventory with
"what healthy looks like" notes, and `gh` recipes for mining past incident RCAs.

Everything there needs the **board lock held first** — see `/hw-lock`; the serial MCP
tools are auto-denied without it. Interact with the shell only via `mcp__serial__*`
tools, never raw `/dev/ttyACM*` (rule and rationale in `fw/CLAUDE.md`).
