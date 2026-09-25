# fw/CLAUDE.md — RGB Sunglasses firmware

Zephyr RTOS / Nordic Connect SDK (NCS v3.1.1) firmware for RGB LED sunglasses on an nRF5340. The codebase is mixed
C/C++; `main.c` is C but most application logic is C++23. Detail for each code area lives in a path-scoped rules file
that loads when you read that code — see the index at the bottom; hardware procedures live in skills.

## Hardware revisions

- `rgb_sunglasses_proto0` is the latest hardware revision and the only board built from `main`. **Enable every new
  feature on proto0 by default** — when asked to add a feature, make sure it is on in the proto0 Kconfig.
- The legacy `rgb_sunglasses_dk` board was removed from `main` (issue #203): its board support and CI live on the frozen
  **`dk-support` branch** (no new features, never merge main into it, never cut `fw-v*` tags from it). Do not add DK
  build steps, board files, or gates on `main`.

## Project vs SDK

- Files under `fw/` are ours to modify. **Files under the NCS SDK (`/root/ncs/v3.1.1` devcontainer, `~/ncs/v3.1.1`
  macOS) are NEVER modified** — touch them only when explicitly requested. Read Zephyr's docs from
  `/root/ncs/v3.1.1/zephyr/doc`.
- The firmware is four images: **MCUboot** (appcore bootloader), **rgb-sunglasses** (`fw`, appcore application),
  **b0n** (netcore bootloader), **ipc_radio** (netcore application).
- **The running firmware lives in INTERNAL flash**; external flash holds settings/bonds and the `/NAND:` asset volume —
  a recurring agent mistake (`.claude/rules/fw-storage-usb.md`).

## Build and test

Use the skills — `/build-proto0`, `/test-fw` — instead of raw `west build`, and `/submit-pr` instead of manually pushing
and opening PRs (it enforces the build, test and **> 70 % patch-coverage** gates). Which skill fits which task: the
root `CLAUDE.md` "Task routing" table (the single one; don't duplicate it here). **Before any `git push` or PR**,
proto0 must build clean (`/build-proto0`) and all tests must pass (`/test-fw`).

| Board | Build dir | When to use |
|---|---|---|
| `rgb_sunglasses_proto0` | `fw/build` | Day-to-day development (incremental) |

Never point a different board at the same build dir — switching boards in one build dir forces a full pristine rebuild.

### Build and Test Commands (raw — prefer the skills above)

```bash
# First time build (pristine, very slow! Only if the build folder is empty/nonexistent — or a newly ADDED
# devicetree overlay, see .claude/rules/fw-sysbuild-mcuboot.md)
west build --build-dir fw/build fw --pristine --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild --cmake-only -- -DCONFIG_DEBUG_THREAD_INFO=y -DBOARD_ROOT="$(pwd)/fw"

# Full incremental build of proto0 (preferred for daily dev)
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# Run all tests on native simulator / a single suite
twister -T fw/tests -p native_sim
twister -T fw/tests/animations/animation_registry -p native_sim
```

- **Run every `west`/`twister`/`pytest` from the repo (or worktree) root, prefixed with an explicit
  `cd "$(git rev-parse --show-toplevel)" &&`** — the agent shell's cwd persists across tool calls, and from inside `fw/`
  these commands fail in ways that look like success (`run_in_background` captures the cwd at launch). After any
  supposedly-green re-run, verify freshness (artifact mtime, or a test count reflecting what you just added)
  (`docs/agent-incidents.md#2026-08-02-stale-cwd-made-a-failed-build-look-green`).
- **Always use `west build`** — never `cmake` or `ninja` directly; raw invocations bypass sysbuild's multi-image
  coordination. Treat a successful `west build` as the first validation step after any change. If a build fails, read
  the log files instead of building again.
- **An incremental build IGNORES a changed Kconfig `default`** — the old `.config` wins silently. `rm
  fw/build/fw/zephyr/.config` (or set the value explicitly in `prj.conf`), and always confirm the value landed in
  `fw/build/fw/zephyr/include/generated/zephyr/autoconf.h` before flashing (`.claude/rules/fw-kconfig-build.md`).
- NCS on the macOS host: `. scripts/fw-env.sh` first (`.claude/skills/flash-and-verify/references/macos-host.md`).

### Known non-blocking build warnings (issue #164 cleanup, 2026-07-17)

A clean proto0 sysbuild still emits these; all are accepted — do not "fix" them, and treat anything NOT on this list as
new and worth investigating:

- `Experimental symbol USB_DEVICE_STACK_NEXT / UDC_DRIVER is enabled` (×2 each) — deliberate choice of the new USB stack
  (see the `fw/prj.conf` comment).
- `Experimental symbol DEBUG_COREDUMP_BACKEND_NRF_FLASH_PARTITION is enabled` — deliberate coredump backend choice.
- `usbd_cdc_acm.c: #warning "USBD_CDC_ACM_LOG_LEVEL forced to LOG_LEVEL_NONE"` — upstream NCS code, not ours to change.

(An old `-Wcomment` note referred to `src/bluetooth/bt_service.h`, which no longer exists; the old `Deprecated symbol
TINYCRYPT is enabled` note no longer applies since `netcore_version.c` moved to stock mbedTLS SHA-256, issue #181.)

## Commenting rules

- **Preserve existing comments.** Never delete comments unless they are factually incorrect about the code that remains
  (e.g., a comment that describes a removed code path). Refactoring an API does not justify removing comments — update
  names in the comment text to match the new API, but keep the explanation.
- **Commented-out code (`/*...*/` or `//`) is intentional.** Developers in embedded projects often comment out
  alternative implementations, debug printk calls, or reference snippets as quick-enable stubs. Do not remove them.
- **Add comments to non-obvious logic.** If the purpose or mechanism of code is not clear from reading it, comment it.
- **Never put a `/*` sequence inside a comment** — glob paths like `/NAND:/ext/*.llext` trip `-Wcomment` ("/* within
  comment"). Rephrase as ".llext files in /NAND:/ext".

## Coding rules

- **Always use bounded string copies** (`strncpy` + explicit NUL, `snprintf`, `memcpy` with a checked length) — never
  `strcpy`/`sprintf`, even when the buffers are provably the same size today (PR #89 review feedback).
- **Never do flash/filesystem I/O from a cooperative-priority thread** — a long flash write starves every other thread.
  Do it from a low-priority workqueue instead (PR #51).
- **Wrap every multi-step I2C/register transaction in a per-device `k_mutex`** (e.g. the TPS25750 I2Cm bridge's
  CMD1/DATA1 sequences), with `_locked` inner functions so every early return releases the lock, and bounded poll loops
  (timeout → `-ETIMEDOUT`) instead of infinite ones. Interleaving corruption shows up as **plausible-but-wrong values**,
  not as I2C errors (PR #111; `.claude/rules/fw-power.md`).
- **No info-level logs in steady-state/per-tick paths** (render ticks, notify calls, poll loops) — they become permanent
  log spam that buries real events (PR #110).
- **Never persist a value on a per-interaction path** (an animation switch, a button press, a shuffle hop) — flash
  endurance is a budget, and a settings write is for a value the user asked to keep (issue #311).
- **A settings-key MISS is orders of magnitude more expensive than a hit**, and the NVS name cache that hides it
  overflows silently — size it against churn (`.claude/rules/fw-settings-persistence.md`).
- **Logging an enum OR a `bool` from C++ prints GARBAGE without an `(int)` cast** — cast every enum (scoped or not) and
  every `bool` log argument; `enum class` is not safe by construction (`.claude/rules/fw-logging.md`).
- **No `%f`/`%g` in log or shell format strings** — float printf is compiled out and prints the literal specifier; use
  integer fixed-point (`.claude/rules/fw-logging.md`).
- **Don't reuse a Kconfig symbol from one subsystem to configure unrelated code in another**, even if the value lines up
  — give it its own `CONFIG_APP_*` symbol (`.claude/rules/fw-kconfig-build.md`).
- **Animations must draw near full-scale (255)** — the global brightness factor scales every pixel down, so a "dim"
  animation is invisible (`.claude/rules/fw-animations.md`).

### Thread priorities and stack sizes

**`fw/docs/threading.md` is the single system-wide map** — every thread, its priority, its stack symbol, and the
invariants between them. Read it before changing a thread priority or adding a thread. Every application thread
priority and stack size is a Kconfig symbol (issue #269); **never re-introduce a bare literal** into a
`K_THREAD_DEFINE` / `K_KERNEL_THREAD_DEFINE` / `k_thread_create` / `k_work_queue_start` call. Ordering invariants are
`BUILD_ASSERT`s next to the threads they constrain. A standalone Twister app does not see `fw/Kconfig` — see
`/add-fw-test` for the test-local `Kconfig`. Converting a thread to user mode: `.claude/rules/fw-userspace.md`.

### SYS_INIT ordering for early registration

`SYS_INIT(fn, APPLICATION, N)` runs before `K_THREAD_DEFINE` threads are scheduled; lower N runs first. When an observer
or listener must be registered before a thread can fire its first event, use `SYS_INIT(APPLICATION, 0)`: both
`bluetooth_init` and `button_init` run at priority 1, so priority-0 registration is in place before either starts.

**C++ static constructors run after POST_KERNEL but BEFORE APPLICATION-level SYS_INIT** (`z_static_init_gnu()` in NCS
v3.1.1's `zephyr/kernel/init.c` `bg_thread_main`, between the two `z_sys_init_run_level()` calls). Any container that
static-init constructors register into — e.g. `fw/src/settings/persistent_value_registry.cpp`, populated by the
`BtGattPersistentCharacteristic` / `ChargeEnableCharacteristic` ctors and the `GlimPersistenceRegistrar` struct — must
be constant-initialized (like that file's `SYS_SLIST_STATIC_INIT` list head), never initialized from an APPLICATION
SYS_INIT, which runs after the ctors and would silently discard every registration.

**SYS_INIT() priority levels must ALWAYS be a plain number or a single preprocessor macro that expands directly to a
number** — never an expression. Both of these are illegal:

```
SYS_INIT(mcuboot_info_init, APPLICATION, CONFIG_RETENTION_BOOTLOADER_INFO_INIT_PRIORITY + 1);

#define MCUBOOT_INFO_INIT_PRIORITY (CONFIG_RETENTION_BOOTLOADER_INFO_INIT_PRIORITY + 1)
SYS_INIT(mcuboot_info_init, APPLICATION, MCUBOOT_INFO_INIT_PRIORITY);
```

To enforce init ordering, use a plain Kconfig value and add `static_assert()`s as needed to guarantee ordering.

## Where the detail lives

Path-scoped rules load automatically when you Read a matching file; Read them by hand when planning (subagents may not
load them).

| Area | Code | Rules file |
|---|---|---|
| BLE wire contract (writes, metadata blob, notify/MTU, conn interval, UUIDs, colors) | `fw/src/bluetooth/`, `fw/src/extensions/extension_bt.cpp`, app | `.claude/rules/ble-gatt-contract.md` |
| GATT server, BT state machine, `bt_state`/`bt_conn_info` | `fw/src/bluetooth.cpp`, `fw/src/bluetooth/` | `.claude/rules/fw-bluetooth-gatt.md` |
| LED pipeline, animations, registry, buttons, pattern controller, GLIM player | `fw/src/animations/`, `fw/src/pattern_controller.cpp` | `.claude/rules/fw-animations.md` |
| Settings persistence, NVS cost, no per-interaction writes | `fw/src/settings/` | `.claude/rules/fw-settings-persistence.md` |
| Extension sandbox runtime, faults, tick budget, `ext` shell | `fw/src/extensions/`, `fw/extensions/` | `.claude/rules/fw-extensions.md` |
| rgbx ABI, SDK, exported-symbol lockstep, API-doc gate | `fw/include/rgbx/`, `fw/sdk/` | `.claude/rules/fw-rgbx-sdk-abi.md` |
| Extension install/list/delete (MCUmgr FS + FILE_MGMT) | `fw/src/extensions/extension_mgmt.cpp`, app | `.claude/rules/extension-file-management.md` |
| Kconfig defaults, CMake gates, memory flags | `fw/Kconfig`, `fw/prj.conf`, board confs | `.claude/rules/fw-kconfig-build.md` |
| Sysbuild overlays, MCUboot VERSION, overwrite-only, LED GPIO hogs | `fw/sysbuild/`, `fw/conf/` | `.claude/rules/fw-sysbuild-mcuboot.md` |
| C++ logging mechanics (enum/bool casts, no `%f`) | `fw/src/**/*.cpp` | `.claude/rules/fw-logging.md` |
| USERSPACE, K_USER conversion, MPU budget | `fw/src/imu/imu.cpp`, board conf | `.claude/rules/fw-userspace.md` |
| TPS25750 / BQ25792 power (SAFE vs DANGER commands) | `fw/src/power.cpp`, `fw/drivers/` | `.claude/rules/fw-power.md` |
| Audio, beat detection, capture files | `fw/src/sound/` | `.claude/rules/fw-sound-capture.md` |
| Flash layout, `/NAND:` volume, patched flashdisk, CDC-ACM FIFO | `fw/src/storage/`, `fw/drivers/flashdisk/` | `.claude/rules/fw-storage-usb.md` |
| Coredumps (capture, drain, fetch, debug) | `fw/src/debug/` | `.claude/rules/fw-coredump.md` |
| Test suites, emulators, HIL suite | `fw/tests/`, `fw/tests_device/` | `.claude/rules/fw-tests.md` |

| Hardware task | Reference |
|---|---|
| Serial shell (ports, `mcp__serial__*`, plugin, quirks, useful commands, ttyACM shifts, wedged UART) | `.claude/skills/flash-and-verify/references/serial-shell.md` |
| J-Link flashing, staged-OTA revert | `.claude/skills/flash-and-verify/references/jlink.md` |
| MCUmgr, image layout, OTA flow | `.claude/skills/flash-and-verify/references/mcumgr.md` |
| macOS host (Mac Mini) | `.claude/skills/flash-and-verify/references/macos-host.md` |
| NAND disk: GLIM assets, mounting, FAT concurrency, reformat | `.claude/skills/provision-device/references/nand-disk.md` |
| IMU coordinate-frame validation | `.claude/skills/capture-scenario/references/imu-frame-validation.md` |
