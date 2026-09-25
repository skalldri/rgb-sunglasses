---
paths:
  - "fw/tests/**"
  - "fw/tests_device/**"
  - "fw/drivers/emul_*/**"
  - "fw/scripts/run-device-tests.sh"
---

# Firmware tests (Twister/ztest on native_sim, and the HIL suite)

Loads when you read a test suite or an emulator. **The file-set and build incantations (dotted
`testcase.yaml` names, `CONFIG_STD_CPP2B`, `-Wno-error=comment`, `DTS_ROOT`, out-of-tree driver
Kconfig/CMake, test-local Kconfig redeclarations, DT filters, native_sim timing) are in
`/add-fw-test`** — don't restate them here. Running the suite + coverage is `/test-fw`.

## Test structure

Tests live under `fw/tests/` as Twister suites using `ztest`; each suite has its own
`CMakeLists.txt`, `prj.conf` and `testcase.yaml`.

- `fw/tests/animations/animation_registry/` — unit tests for the registry itself.
- `fw/tests/animations/*_animation_di/` — dependency-injection tests per animation, compiling the
  pure animation `.cpp` without BT; every built-in animation has one. **No DI suite sets
  `CONFIG_BT`** — the suites that do are under `fw/tests/bluetooth/` (check with
  `grep -rln CONFIG_BT=y fw/tests --include=prj.conf` from the repo root).
- `fw/tests/bt_state_observer/` — `BtStateObserver` contract tests (does not link `bluetooth.cpp`).
- `fw/tests/configuration_provider/` — `ConfigurationProvider` contract tests.
- `fw/tests/power/tps25750_patch_decompression/` — the LZ4-compressed TPS25750 patch round-trips.
- `fw/tests/drivers/emul_bmi270/` — exercises the **real upstream bmi270 driver** on native_sim
  through the out-of-tree BMI270 emulator at `fw/drivers/emul_bmi270/` (upstream Zephyr has no
  BMI270 emul; ours is SPI-only, matching proto0). Two scenarios: poll mode, and
  `CONFIG_BMI270_TRIGGER_OWN_THREAD` where the data-ready trigger is fired by toggling INT2
  (**irq-gpios index 1** — the driver maps data-ready to INT2, not INT1) via native_sim's
  `gpio_emul`. Tests inject SI-unit samples with `emul_sensor_backend_set_channel()` and peek
  registers via `emul_bmi270_get_reg()`. `CONFIG_EMUL_BMI270` depends on `EMUL && BMI270` so
  hardware builds never compile it. Deferred follow-ups: I2C support, bad-chip-id failure path.
- `fw/tests/drivers/emul_tps25750/` — exercises the **real tps25750 + bq25792 drivers** through the
  out-of-tree TPS25750 emulator at `fw/drivers/emul_tps25750/`: the bq25792 node is a DT child of the
  tps25750 node (same topology as proto0), so every BQ register access runs through the real I2Cm
  bridge (CMD1/DATA1 4CC tasks + `task_mutex`) into the emulated register file. Two scenarios:
  default (boots in "APP " mode), and `.patch_download` (`CONFIG_EMUL_TPS25750_BOOT_MODE_PTCH=y` +
  `CONFIG_TPS25750_INTERNAL_PATCH=y`, asserting the full boot-time PBMs → chunked upload → PBMc flow
  via received-byte count + FNV-1a hash). Non-obvious mechanics: (1) patch chunks arrive on a
  **second I2C address** (the DT `patch-address`), which `EMUL_DT_DEFINE` can't cover — the
  emulator's init hand-registers an extra `struct i2c_emul` via `i2c_emul_register()`; (2) the
  emulated CMD1 must stay **busy for a nonzero window** (`emul_tps25750_set_cmd_delay_ms`) or a
  bridged transfer has no blocking point on native_sim, threads never interleave, and the
  concurrency regression test (for the `task_mutex` fix) can't reproduce the race — validated by
  disabling the mutex: 17/100 concurrent reads then return the *other* register's value; (3) the test
  app needs `list(APPEND DTS_ROOT ...fw)` before `find_package(Zephyr)` for the custom
  `ti,tps25750`/`ti,bq25792` bindings; (4) all BQ getters (`bq25792_get_*`) propagate I2C errors, so
  error-path tests can assert getter errnos directly (driving the bridge with
  `i2c_burst_read(tps_dev, 0x6B, ...)` also works).
- `fw/tests/drivers/bq25792_decode/` — the ADC sign-extension regression (PR #106).
- `fw/tests/imu/pipeline/` — end-to-end test of `fw/src/imu/imu.cpp` (compiled into the test app)
  against the BMI270 emulator: boot-time ODR/power config, DRDY-driven frames into `imu_result_q`,
  and the msgq purge-keep-freshest overflow path. Timing: the bmi270 driver's per-register
  `k_usleep` rounds each to a 10 ms tick on native_sim, so `imu_thread`'s startup config takes
  ~200 ms simulated (the setup polls PWR_CTRL for completion) and DRDY pulses need ~50 ms spacing or
  they coalesce on the driver's trigger semaphore and frames drop. `imu.cpp`'s two `fw/Kconfig`
  symbols (`IMU_THREAD_PRIORITY`/`IMU_THREAD_STACK_SIZE`) are redeclared in the test-local `Kconfig`.
- `fw/tests/debug/coredump_manager/` — the coredump drain logic behind a `PartitionOps` seam
  (`DEBUG_COREDUMP` doesn't exist on native_sim; `.claude/rules/fw-coredump.md`).
- Any suite that compiles a thread-owning `.cpp` needs a test-local `Kconfig` redeclaring its
  priority/stack symbols with matching defaults — see `fw/tests/led_controller/Kconfig` and
  `fw/tests/imu/pipeline/Kconfig`.

**Pulling out-of-tree drivers into a standalone test app** (used by `tps25750_patch_decompression`
and `emul_bmi270`) needs no Zephyr module registration — just the test-local `Kconfig` +
`add_subdirectory` pattern in `/add-fw-test`. Driver dirs use `zephyr_sources()`, not
`zephyr_library()` (see the comment in `fw/drivers/vm3011/CMakeLists.txt`).

**Test isolation from heavy dependencies**: if a registration function (e.g.
`bluetooth_register_state_observer`) lives in a file with heavy BT stack dependencies, don't link
that file in unit tests. Test the interface/observer contract directly on a mock implementation.

## On-device (HIL) test suite

`fw/tests_device/` runs pytest suites against the real production sysbuild image on a flashed
proto0, via `twister --device-testing` + the pytest-twister-harness `shell`/`dut` fixtures. Entry
point: `fw/scripts/run-device-tests.sh` (hold the `board` lock first, same as flashing).
Architecture + CI north-star: `fw/docs/on-device-testing.md`; tier semantics and house rules:
`fw/tests_device/README.md`. It complements — never replaces — the native_sim suites; anything
testable on native_sim belongs there. Its `no_staged_ota` fixture (`fw/tests_device/conftest.py`)
guards against the staged-OTA revert (`.claude/skills/flash-and-verify/references/jlink.md`).
