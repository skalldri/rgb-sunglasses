---
name: add-fw-test
description: Add a firmware Twister/ztest test suite (native_sim, no hardware) or fix a failing one — DI animation tests, driver emulator tests, GATT service tests, FAT/ramdisk tests. Includes the exact prj.conf/testcase.yaml/CMakeLists incantations that fail cryptically if wrong.
---

# Add a firmware Twister test suite

All firmware tests are ztest suites run by Twister on `native_sim` — no hardware, no
lock, no cross-compile. Read `fw/CLAUDE.md` first if you haven't.

**Golden rule: copy an exemplar, don't invent.** Every pattern you need already exists
in-tree (table below). Inventing config from memory is how you hit the cryptic
failures in the Incantations box.

## Minimal file set

Create `fw/tests/<area>/<name>/` containing exactly:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | `find_package(Zephyr ...)`, `target_sources(app PRIVATE src/main.cpp)` + each real unit-under-test `.cpp` via `${CMAKE_CURRENT_LIST_DIR}/../../../src/...`, `target_include_directories(app PRIVATE .../src)` |
| `prj.conf` | `CONFIG_ZTEST=y` + C++/feature configs (see Incantations) |
| `testcase.yaml` | scenario name(s), platforms, filters |
| `src/main.cpp` | `#include <zephyr/ztest.h>`, fakes, `ZTEST_SUITE(...)`, `ZTEST(...)` cases. Multiple `ZTEST_SUITE`s per binary are fine. |

Optional, only when needed: `Kconfig` (out-of-tree driver symbols or `fw/Kconfig`
mirrors), `boards/native_sim.overlay` (ramdisk / flash partitions / emulated devices),
`include/` (mock headers).

## Exemplars — copy the closest match

| You are testing... | Copy from | Key techniques |
|---|---|---|
| Pure logic / a registry (minimal suite) | `fw/tests/animations/animation_registry/` | 4-file minimum; compiles real `.cpp` directly |
| An animation via DI interfaces | `fw/tests/animations/zigzag_animation_di/` | Hand-rolled fakes in `src/main.cpp`: `MutableUint32Source` (implements `AnimationUint32ParameterSource`), `CapturingTestRenderer` (implements `AnimationRenderer`, records pixel writes) |
| A GATT service with real BT host headers | `fw/tests/bluetooth/battery_service/` | `CONFIG_BT=y`/`CONFIG_BT_PERIPHERAL=y`, `bt_enable()` never called; finds the attr table via `STRUCT_SECTION_FOREACH(bt_gatt_service_static, ...)`; driver replaced by `extern "C"` recording fakes; placeholder DT device via `DEVICE_DT_DEFINE(DT_NODELABEL(bq25792), ...)`; test-local `Kconfig` mirrors `CONFIG_APP_*` symbols |
| A driver against a full I2C/SPI emulator | `fw/tests/drivers/emul_tps25750/` | Custom DT bindings via `DTS_ROOT`, `rsource` driver Kconfig, `extra_configs:` second scenario |
| FAT filesystem / ramdisk I/O | `fw/tests/storage/glim_decoder/` | 2 suites in one binary (pure + `_io` with mkfs/mount fixtures); `boards/native_sim.overlay` with a `zephyr,ram-disk` node |
| Code needing partition-manager macros | `fw/tests/mcuboot_updater/` | Mock `include/pm_config.h` + `filter:` + flash-partition overlay |

**One home per unit.** If the unit under test already has incidental coverage bolted
onto another suite, migrate those cases into the new dedicated suite and delete them
from the foreign file (re-running the foreign suite to confirm it still passes) —
never leave the same unit tested in two places.

**Mocking policy: this repo uses NO fff (Fake Function Framework), ever.** The four
sanctioned styles: (a) C++ interface fakes implementing DI abstract classes, (b)
`extern "C"` link-time recording fakes, (c) DT-registered device emulators
(`fw/drivers/emul_*`), (d) mock headers winning by include-path precedence.

## Incantations (each fails cryptically if wrong)

- **testcase.yaml scenario names MUST be dotted** `category.name` (e.g.
  `animations.foo_animation_di`). A single-word name throws `TwisterException` at
  RUNTIME, not parse time.
- **C++ sources** need in prj.conf: `CONFIG_ZTEST=y`, `CONFIG_CPP=y`,
  `CONFIG_STD_CPP2B=y` (**`CONFIG_STD_CPP23` does not exist**),
  `CONFIG_REQUIRES_FULL_LIBCPP=y`, `CONFIG_REQUIRES_FULL_LIBC=y`.
- Any TU that pulls in `bt_service_cpp.h` (directly or via `bluetooth/` headers) trips
  `-Wcomment` as an error — add
  `target_compile_options(app PRIVATE -Wno-error=comment)`.
- Custom DT bindings (`ti,tps25750`, `ti,bq25792`, ...) need
  `list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)` **BEFORE**
  `find_package(Zephyr ...)` or the bindings are silently not found.
- **Out-of-tree drivers**: test-local `Kconfig` starting with
  `source "Kconfig.zephyr"` then `rsource "../../../drivers/Kconfig"`; in CMakeLists
  `add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../drivers ${CMAKE_CURRENT_BINARY_DIR}/app_drivers)`
  (and `.../include` → `app_include` for public driver headers).
- **`fw/Kconfig` app symbols are invisible to standalone twister apps.** If the unit
  consumes `CONFIG_APP_*` / `CONFIG_IMU_*`, redeclare them (same name/default) in the
  test-local `Kconfig` — see `fw/tests/imu/pipeline/Kconfig` and
  `fw/tests/bluetooth/battery_service/Kconfig`. Forgetting the
  `source "Kconfig.zephyr"` first line breaks the entire Kconfig tree.
- Mock headers only beat generated ones with
  `target_include_directories(app BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)` —
  plain `PRIVATE` ordering is not enough (see `fw/tests/mcuboot_updater/CMakeLists.txt`).
- **DT-dependent suites need a `filter:`** in testcase.yaml, e.g.
  `filter: dt_label_with_parent_compat_enabled("settings_storage", "fixed-partitions")`
  (see `fw/tests/storage/appcfg_erase/testcase.yaml` and
  `fw/tests/mcuboot_updater/testcase.yaml`), so they skip instead of build-breaking
  where the nodes don't exist.
- Scenarios normally carry `platform_allow: [native_sim]` +
  `integration_platforms: [native_sim]`. Two in-tree suites
  (`fw/tests/bt_state_observer`, `fw/tests/configuration_provider`) have only
  `integration_platforms` — that's deliberate; don't "fix" them reflexively.

## Run it

From the repo root (`twister` is on PATH):

```bash
# Just your new suite
twister -T fw/tests/<area>/<name> -p native_sim --outdir fw/twister-out-one
# One scenario of a multi-scenario testcase.yaml
twister -T fw/tests/drivers/emul_tps25750 -p native_sim -s drivers.emul_tps25750.patch_download --outdir fw/twister-out-one
```

The scratch `--outdir` follows `/test-fw`'s targeted-run rule (see that skill for why
the default outdir behavior bites).

Full run + lcov coverage: use the `/test-fw` skill (don't retype its command).
Execution is seconds; builds dominate the wall clock — don't run the full suite just
to "check timing".

Per-suite output lands in
`<outdir>/native_sim_native/host/<suite-rel-path>/<scenario>/`: `build.log` (compile
failures), `handler.log` (test stdout), `zephyr/zephyr.exe` (runnable binary). To
debug a failure by executing `zephyr.exe` directly / under gdb, use the `/debug-fw`
skill.

## Coverage gate (why you're probably here)

`/submit-pr` gates on ≥50% patch coverage and counts an **empty lcov extract as 0%**:
a changed `.cpp` compiled by no test suite blocks the PR outright. The fix is exactly
this skill — add a suite that compiles that file via `target_sources`. Open issue #83
(`imu.cpp` / `sound.cpp`, as of 2026-07 — re-verify) is precisely this situation.

Note: not every animation has a DI suite (e.g. `matrix_code` has none) — parity is
not guaranteed or required; add suites where coverage is needed.

## native_sim gotchas

- **Time is simulated and coarse**: `k_usleep` rounds up to full 10 ms ticks (see the
  comments in `fw/tests/imu/pipeline/src/main.c` — driver boot takes ~200 ms of
  simulated time). Poll for readiness in suite setup; never sleep fixed "real-world"
  amounts. Space emulated interrupts ~50 ms apart — the sleep must outlast the whole
  trigger-thread + fetch + msgq-put chain (several ticks), not merely exceed one
  tick, or pulses coalesce and frames drop (see `fire_drdy()` in that file).
- **Concurrency tests need an artificial blocking window.** On native_sim a transfer
  with no blocking point never interleaves threads, so races won't reproduce. Use the
  emulator hook (`emul_tps25750_set_cmd_delay_ms()` in
  `fw/drivers/emul_tps25750/emul_tps25750.h`) to hold a command busy while a second
  thread collides — see `fw/tests/drivers/emul_tps25750/src/main.cpp`.
