---
paths:
  - "fw/src/debug/**"
  - "fw/src/extensions/sandbox_fatal_handler.cpp"
  - "fw/scripts/coredump-*.sh"
  - "fw/tests/debug/**"
---

# Coredumps (issue #80, proto0 only)

Loads when you read the coredump/fault-handling code or scripts. `/debug-fw` routes crash symptoms
here.

A fatal fault captures a Zephyr coredump to the 64 KB internal-flash `coredump_partition` (0xF0000)
via the NCS `DEBUG_COREDUMP_BACKEND_NRF_FLASH_PARTITION` backend — raw `nrfx_nvmc` pokes, the only
flash path that works inside the fault handler (IRQs locked; the external QSPI driver needs
interrupts/scheduler, so dumps can NEVER target external flash directly). `z_fatal_error()` writes
the dump BEFORE calling `k_sys_fatal_error_handler`, so extension-sandbox faults produce dumps too
even though the handler demotes them to a thread abort.

**Post-fault behavior** (`k_sys_fatal_error_handler` in `fw/src/extensions/sandbox_fatal_handler.cpp`):
sandbox faults → thread abort; anything else → cold reboot, UNLESS a debugger is attached (DHCSR
C_DEBUGEN), in which case it halts for GDB. Expect a ~2 s freeze during capture (16-page partition
erase + write, IRQs locked) — including on recoverable sandbox faults.

**Drain** (`fw/src/debug/coredump_manager.cpp`, `CONFIG_APP_COREDUMP_MANAGER`): every
`CONFIG_APP_COREDUMP_REMINDER_PERIOD_S` (60 s; first check ~5 s after boot) a dedicated workqueue
checks the partition, copies any verified dump to `/NAND:/coredump/core_NNNN.bin`, and invalidates
the partition. **There is no recurring "awaiting collection" reminder** — it was removed because it
re-logged every 60 s forever on any board carrying an uncollected dump. Check on demand with
`coredump_mgr status`, and collect with `fw/scripts/coredump-fetch.sh` (`--delete` frees the space;
the board must be rebooted after — FAT concurrent access,
`.claude/skills/provision-device/references/nand-disk.md`).

**That 60 s period is a data-loss window, not just a poll interval.** The NCS flash backend erases
the whole coredump partition at the *start* of every capture (`coredump_flash_backend_start()` →
`flash_area_flatten()`), so the next crash is always captured — what the drain rescues is the
*previous* dump. A second fault inside the period destroys the first one, which on a boot-looping
board is the dump you actually wanted. Do not raise it to quiet logs. The pure logic lives in
`coredump_manager_core.cpp` behind a `PartitionOps` seam so `fw/tests/debug/coredump_manager` covers
it on native_sim (where `DEBUG_COREDUMP` doesn't exist).

## Fetch + debug from the host

```bash
fw/scripts/coredump-fetch.sh --delete ./dumps   # mount MSC disk, copy core_*.bin off
fw/scripts/coredump-debug.sh dumps/core_0000.bin  # gdbserver --pipe + arm-zephyr-eabi-gdb, prints bt
```

The dump files are the raw Zephyr coredump stream ("ZE" magic) that `coredump_gdbserver.py`
consumes directly. The ELF passed to `coredump-debug.sh` must be from the build that produced the
crash. Serial fallback when USB is unavailable: `coredump print` on the shell, then
`coredump_serial_log_parser.py` on the captured log. The built-in `coredump find/verify/print/erase`
shell commands are enabled on proto0.

**Test commands**: `crash panic` (kernel panic) and `crash mpu` (write to RO flash → MemManage
fault), `CONFIG_APP_CRASH_TEST_COMMANDS`. Full loop: `crash panic` → reboot → within ~5 s the manager
logs `coredump ... saved to /NAND:/coredump/core_0000.bin` → fetch + debug → the GDB backtrace shows
`cmd_crash_panic` on the shell thread.

## Dump-size budget — 64 KB is a hard cap, not a truncation point

The NCS backend drops the ENTIRE dump if it doesn't fit (`-ENOMEM`, header never written, nothing to
find on reboot). The budget is enforced by `DEBUG_COREDUMP_THREAD_STACK_TOP_LIMIT=1536` (each
thread's stack dumped only from SP down, capped at 1536 B) — worst case ~55 KB at ~27 threads; the
arithmetic lives next to the Kconfig in `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`. Redo
it before raising the limit or adding many threads. (The legacy DK board, now on `dk-support`, has
no coredump support.)
