---
name: rom-ram-budget
description: Check, verify, or recover firmware FLASH/RAM usage — linker overflow on DK, RAM% jumps, sizing a new feature, right-sizing stacks. Every size claim must be verified against the linker map, not footprint scripts.
allowed-tools: Bash, Read, Grep, Glob
---

# Firmware FLASH/RAM budget

## Ground truth: the linker map, nothing else

The only numbers that govern link success are the linker's. Per root `CLAUDE.md`
("Working with hardware"), verify every memory-accounting claim against:

- proto0: `fw/build/fw/zephyr/zephyr.map`
- DK: `fw/build-dk/fw/zephyr/zephyr.map`

The linker's RAM% **counts `.noinit` buffers** (e.g. the llext heap,
`CONFIG_LLEXT_HEAP_SIZE` in `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`).
Footprint reports (`west build -t ram_report` / `rom_report`, CI footprint scripts) use
different accounting and will disagree — never cite them for a link-success argument.
The summary lines land at the end of build output as
`Memory region  Used Size  Region Size  %age Used` with `FLASH:` and `RAM:` rows —
one block per image; the appcore app image is the one that matters here.

## Current envelope — historically observed as of 2026-07, re-verify from build output

- **DK appcore FLASH: 92–94%.** A change that is safe on proto0 can overflow the DK
  (imgtool `Image size ... exceeds requested size`). This is why `/submit-pr` builds
  BOTH boards; do the same in ad-hoc checks.
- **proto0 appcore RAM history:** 75.3% (PR #81) → 90.5% (USERSPACE #82 + LLEXT #89)
  → 76.2% (recovery pass PR #103). proto0 FLASH was ~66% after #103.
- Do not treat any of these as current facts — rebuild and read the real numbers.

## Cost catalog (from PRs #81, #82, #103; issue #84)

| What | Cost / saving | Where verified |
| --- | --- | --- |
| `K_THREAD_STACK_DEFINE` under `CONFIG_USERSPACE` | silently +1KB privileged stack per thread | PR #103; `priv_stacks` in zephyr.map |
| `CONFIG_SIZE_OPTIMIZATIONS=y` (`-Og`→`-Os`) | saved ~255KB FLASH | PR #82; already `=y` in both board confs |
| Template-heavy `fw/src/bluetooth/bt_service_cpp.h` instantiations | ≈70KB FLASH | issue #84 (open) |
| DK feature shedding | frees DK FLASH | `fw/boards/rgb_sunglasses_dk_nrf5340_cpuapp.conf` |

Details:

- **Kernel-only threads must use `K_KERNEL_THREAD_DEFINE` / `K_KERNEL_STACK_DEFINE`.**
  Every statically-defined app thread already does this deliberately
  (`grep -rn K_KERNEL_THREAD_DEFINE fw/src` — power, status_led, sound, pattern
  controller, bluetooth, led_controller; workqueue stacks in mcuboot_updater and
  persistent_value_store). The only `K_THREAD_STACK_DEFINE`s left are the two K_USER
  threads (`fw/src/imu/imu.cpp`, `fw/src/extensions/extension_host.cpp`) — those are
  correct as-is. Don't "normalize" in either direction.
- **DK-only FLASH overflow → the sanctioned fix is shedding features in
  `fw/boards/rgb_sunglasses_dk_nrf5340_cpuapp.conf`** via `=n` lines. Existing
  precedent in that file: `CONFIG_MCUMGR_GRP_*=n`, `CONFIG_APP_PERSIST_BT_CONFIG=n`,
  `CONFIG_APP_BT_METADATA_CHARACTERISTIC=n`, `CONFIG_APP_MCUBOOT_INFO_SERVICE=n` (+
  retention stack). DK is legacy and doesn't get new features (per `fw/CLAUDE.md`), so
  cutting there is acceptable; follow the file's comment style (each `=n` explains why).
- **`default y` Kconfig danger:** a new feature symbol in `fw/Kconfig` with a bare
  `default y` lands on BOTH boards, including the nearly-full DK. Either gate it
  (`default y if BOARD_RGB_SUNGLASSES_PROTO0_NRF5340_CPUAPP` + `default n` — precedent:
  `APP_MCUBOOT_UPDATER`) or add an `=n` override to the DK board conf.

## Procedure

1. **Build** with `/build-proto0` and `/build-dk` (never duplicate their commands; they
   also state the report/warn thresholds). Both boards, always — a proto0-only check
   is how DK overflows land.
2. **Read the FLASH/RAM `%age Used` lines** from the end of each build's output.
   For deltas, compare against a pre-change build of the same board/build dir.
3. **For any claim about a specific symbol or section** (e.g. "sSlots costs 14.5KB",
   "the llext heap is in .noinit"), grep the map:
   ```bash
   grep -n 'sSlots' fw/build/fw/zephyr/zephyr.map
   grep -n 'priv_stacks\|noinit' fw/build/fw/zephyr/zephyr.map | head
   ```
   The map shows section, address, size, and the object file that contributed it.
4. **Never propose a size/config change without a zephyr.map citation** (symbol + size
   from the map, not from a footprint report or from memory).
5. **Report deltas in the PR body** per house style — see PR #103 for the model: a
   table of change → RAM/FLASH saved, each map-verified, plus before/after percentages
   in the Pre-PR checks (`proto0 build: PASS (RAM x%, FLASH y%)`, `DK build: PASS
   (FLASH z%, no overflow)`).

## Known open right-sizing tasks (safe starter work)

- **Issue #105** — `CONFIG_MAIN_STACK_SIZE=32768` (`grep -n CONFIG_MAIN_STACK_SIZE
  fw/prj.conf`). PR #81 measured a 24.8KB high-water mark (deep static-init paths);
  after PR #103's FontAtlas fix the on-device high-water dropped to 7.5KB. The issue
  body has the investigation plan; note its measurement steps (`kernel stacks` on the
  serial shell) need hardware — the offline part (`-fstack-usage` walk) does not.
- **Issue #104** — extension slot arrays `sBtSlots` (`fw/src/extensions/extension_bt.cpp`)
  + `sSlots` (`fw/src/extensions/extension_host.cpp`), ~46KB static worst-case
  (16 slots × 16 params). The issue body contains a full pooled-allocator design;
  hardware verification is mandatory for that one.

## K_USER conversion warning

**Never convert a `K_THREAD_DEFINE` thread to K_USER in place.** On this SoC,
statically-defined threads have a zeroed `mem_domain_info`, and
`k_mem_domain_add_thread()` faults on them; a converted thread also needs
`z_libc_partition` in its memory domain or it usage-faults on its first instruction.
Both crash classes are documented in `fw/CLAUDE.md` ("CONFIG_USERSPACE / kernel-user
mode separation"). Copy the working pattern from `imu_init()` in `fw/src/imu/imu.cpp`:
`K_THREAD_STACK_DEFINE` + `k_thread_create(..., K_FOREVER)` from a SYS_INIT hook,
access grants, `k_mem_domain` with `{own_partition, z_libc_partition}`, then
`k_thread_start()`.

## Validation

Both boards build clean (`/build-proto0`, `/build-dk`) and `/test-fw` passes. Any
change touching stack sizes or thread definitions of hardware-bound subsystems also
needs on-device verification before PR — that path goes through `/submit-pr`, which
owns the hardware gates; this skill has no hardware steps.
