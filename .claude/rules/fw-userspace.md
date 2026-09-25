---
paths:
  - "fw/src/imu/imu.cpp"
  - "fw/src/extensions/extension_host.cpp"
  - "fw/src/extensions/sandbox_*"
  - "fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf"
  - "fw/docs/threading.md"
---

# CONFIG_USERSPACE / kernel-user mode separation (issue #79, proto0 only)

Loads when you read the user-mode threads or the board conf. `CONFIG_USERSPACE=y` is set in
`fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf` (never on the legacy DK board, now on the
`dk-support` branch). Thread map: `fw/docs/threading.md`.

## FLASH cost

`CONFIG_USERSPACE=y` alone (no threads converted) costs ~105-246 KB — mostly the `z_vrfy_*`/`z_mrsh_*`
syscall verifier functions Zephyr generates for every syscall-covered API already compiled in (GPIO,
I2C, SPI, flash, sensor, LED, …), regardless of whether anything calls them from user mode. It is
generated per Kconfig-enabled subsystem, not per actual usage — **there is no way to emit syscalls
for only the subsystems a converted thread needs** (confirmed by reading
`parse_syscalls.py`/`gen_syscalls.py`/`syscall_dispatch.c`); `CONFIG_EMIT_ALL_SYSCALLS` only widens
emission. Fitting it required a flash-reduction pass first (history:
`.claude/skills/rom-ram-budget/references/rom-pass-history.md`).

## Converting a thread to `K_USER` — two crashes, both root-caused via GDB+SWD

`imu_thread` (`fw/src/imu/imu.cpp`) was the first thread converted; the extension sandbox thread
(`fw/src/extensions/extension_host.cpp`) and the Wasm3 runtime thread followed the same recipe. USB
never enumerates when either crash happens at boot, so serial logs aren't available.

1. **`K_THREAD_DEFINE` + `k_mem_domain_add_thread()` crashes on this SoC.** This config has
   `CONFIG_ARCH_HAS_CUSTOM_SWAP_TO_MAIN=1`, so `K_THREAD_DEFINE` (static) threads are set up with
   `_current == NULL` — this skips `z_mem_domain_init_thread()` and leaves the thread's
   `mem_domain_info` permanently zeroed (never linked into `k_mem_domain_default` or any domain).
   `k_mem_domain_add_thread()` unconditionally unlinks the thread from its prior domain first
   (`remove_thread_locked()` → `sys_dlist_remove()`), which faults on that never-linked node.
   **Fix: create the thread dynamically** — `K_THREAD_STACK_DEFINE` + `struct k_thread` +
   `k_thread_create(..., K_FOREVER)` from a SYS_INIT hook (so `_current` is a real, domain-linked
   thread), do the access-grant/domain setup, then `k_thread_start()`. Matches Zephyr's
   `samples/userspace/prod_consumer/src/app_a.c`. This is project-wide: **every** `K_THREAD_DEFINE`
   thread here has the same zeroed `mem_domain_info` (confirmed via GDB on `status_led_thread` too) —
   a conversion needs dynamic creation, not just a `K_USER` flag.
2. **A converted thread also needs `z_libc_partition`, not just its own partition.** `z_arm_tls_ptr`
   (the current thread's TLS pointer, read by every thread at entry via `__aeabi_read_tp()` since
   `CONFIG_CURRENT_THREAD_USE_TLS=y`) lives in `z_libc_partition`, part of `k_mem_domain_default` —
   so every thread has it until moved to a custom domain. Moving a thread to its own domain silently
   drops that access unless `z_libc_partition` (`#include <zephyr/sys/libc-hooks.h>`) is added
   alongside the thread's own partition. Without it: a usage fault on the first instruction of
   `z_thread_entry()`, before the thread's entry function starts.

`imu_init()` in `fw/src/imu/imu.cpp` is the reference implementation of both fixes.

## Threads that stay kernel-mode

Missing syscall coverage in NCS v3.1.1 (confirmed by grepping `__syscall` across the headers this
project uses, not inferred): the Bluetooth host stack, the USB device_next stack, mcumgr, the
settings subsystem, the filesystem API (`fs_*`), `flash_area_*`, `dmic_*`, and the WS2812
`led_strip_update_rgb()` call all have **zero** `__syscall` wrappers. `bt_thread`,
`audio_dsp_thread` (also does raw MMIO pokes for AGC gain), the MCUboot updater work queue and the
settings-save work queue stay kernel-mode barring upstream syscall additions (the SDK is never
modified). `led_display_thread`/`status_led_thread` are simple but blocked on
`led_strip_update_rgb` — they would need one small project-defined syscall wrapper.
`pattern_controller_thread` would need its FS (`glim_registry::init()`) and settings
(`persistent_value_store::request_save()`) calls hoisted into IPC to a kernel-mode helper first.

That per-thread conversion plan is **superseded for the LLEXT case** by the extension-host design
(issue #85): only extension code runs in user mode; `pattern_controller_thread`,
`led_display_thread` and `status_led_thread` stay kernel-mode, so no `led_strip_update_rgb` syscall
or FS hoisting is needed (`.claude/rules/fw-extensions.md`).

## MPU region budget

nRF5340's Cortex-M33 MPU has 8 hardware regions; 2 are permanently consumed by Zephyr's default
flash/RAM background map, leaving ~6 dynamic regions (~4-5 usable partitions per active memory
domain in practice) — a real constraint for anyone adding domains. The extension sandbox uses 5.
