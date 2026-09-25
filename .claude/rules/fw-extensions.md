---
paths:
  - "fw/src/extensions/**"
  - "fw/extensions/**"
  - "fw/tests/extensions/**"
---

# Sandboxed animation extensions (issue #85, `fw/src/extensions/` + `fw/extensions/`)

Loads when you read extension-host or in-repo extension code. Writing an extension is
`/add-extension`; developer docs are `fw/extensions/README.md` (API docs published at
<https://rgb-sunglasses.autom8ed.com/api>). The rgbx ABI/SDK lockstep rules are in
`.claude/rules/fw-rgbx-sdk-abi.md`; file transfer + FILE_MGMT (install/list/delete) in
`.claude/rules/extension-file-management.md`; param persistence in
`.claude/rules/fw-settings-persistence.md`; the user-mode mechanics in `.claude/rules/fw-userspace.md`.

## Architecture

`.llext` animation extensions are discovered at boot from `/NAND:/ext/` and executed
**exclusively on one K_USER sandbox thread** confined to a single shared memory domain
re-initialized per activation (`z_libc_partition` + llext's 4 TEXT/RODATA/DATA/BSS partitions =
5, hardware-verified to fit the MPU budget). The kernel-side pattern controller exchanges data
purely through the extension's own exported globals (ABI in `fw/include/rgbx/rgbx_api.h` — 16
params of type UINT32/COLOR/BOOL/STRING/FLOAT, IMU + audio + button inputs; C++ wrapper
`fw/include/rgbx/rgbx_animation.h`). FLOAT rides in the shared u32 slot as raw IEEE-754 bits
(added with NO ABI version bump — layout unchanged); non-finite values are rejected at every
write/restore path via `extension_manifest::f32_bits_non_finite`.

The host enforces a per-tick **CPU** budget and recovers from hangs/faults by tearing the sandbox
down. Extensions appear as first-class animations: runtime GATT services
(`fw/src/extensions/extension_bt.cpp`, `CONFIG_BT_GATT_DYNAMIC_DB=y`, ids `0x40 + slot`,
capacity/ID constants + static_asserts in `extension_limits.h`) that the app renders with zero
app-side changes, including the bulk metadata characteristic (`.claude/rules/ble-gatt-contract.md`).
Manifest validation is a pure function (`extension_manifest.cpp`, the `extensions.manifest`
native_sim suite) — every manifest-embedded pointer is untrusted and bounds-checked before any
kernel-mode dereference.

## Non-obvious facts learned the hard way

- **Load-on-activate**: boot discovery loads each ELF transiently (validate + copy metadata), then
  unloads; only the ACTIVE extension is llext-resident. `activate()` (often on the BT RX thread)
  only queues the load — the pattern-controller thread performs the FAT read + relocation + sandbox
  bring-up lazily on the first `tick()`, so an `rgbx_init` failure is reported *asynchronously*
  (fault + Is Active notify), not as an `activate()` return value. Consequence: the heap only needs
  the largest single extension (`CONFIG_LLEXT_HEAP_SIZE=24` KB, in the proto0 board conf) and
  16 slots fit.
- **The llext heap buffer is `.noinit` but IS counted in the linker's RAM percentage** (verified in
  `zephyr.map` — don't "discover" 64 KB of free RAM that isn't there).
- **`k_sys_fatal_error_handler` is overridden in `fw/src/extensions/sandbox_fatal_handler.cpp`**
  (it moved there from `extension_host.cpp` when the Wasm runtime gained the same containment;
  root-caused via GDB+SWD). Zephyr's default weak handler halts the WHOLE system on any fault —
  `z_fatal_error()` only demotes to a thread abort if the handler *returns*. The override returns
  only for faults on the sandbox thread; any other fault cold-reboots, unless a debugger is attached
  (DHCSR C_DEBUGEN), in which case it halts for GDB (`.claude/rules/fw-coredump.md`). Without the
  override, an extension MPU fault parked the CPU in `arch_system_halt()`.
- **C++ extensions require a partial link** (`ld -r`, done by `fw/extensions/build.sh`): COMDAT
  group sections (`.text._Z...`) interleave with `.data`/`.bss` file offsets in a single object and
  fail llext's region-overlap check ("Region 0 ELF file range ... overlaps with 1").
- **The `llext-edk` cmake target does not rebuild when headers change** — delete
  `fw/build/fw/zephyr/llext-edk.tar.xz` first (`build.sh` does).
- **Extension init arrays run inside the sandbox** via `llext_bringup()` from the user-mode thread
  entry (`llext_get_fn_table` is a syscall) — needed for C++ static constructors, though GCC
  constant-initializes simple instances (vtable pointer via `.rel.data`).
- Re-initializing the shared `k_mem_domain` is safe **only after the sandbox thread is aborted**
  (`k_mem_domain_init` fully resets the object; `k_thread_abort` unlinks the thread) — every
  teardown path preserves that order.
- Animations must draw near full-scale — see `.claude/rules/fw-animations.md` "Brightness".

## Debug shell

`ext list` / `ext select <slot>` / `ext param <slot> <idx> [<value>]` (type-aware: bools 0/1,
strings as text) / `ext stats` (per-tick **cpu** and **wall** min/avg/max µs, printed separately) /
`ext faults` + `ext faults clear`. The hello kitchen-sink demo doubles as the recovery test
(`Crash`/`Hang` bool params). Printing floats: `fmt_param_f32()` in `extension_host.cpp` (no `%f`,
`fw/CLAUDE.md` "Coding rules").

- **`ext faults` is the post-mortem surface** (issue #308): the fault reason is otherwise a single
  `LOG_ERR` that scrolls out of the UART backlog, and boot-time faults may never reach a console at
  all (the USB CDC backend attaches seconds into boot, after `CONFIG_LOG_BUFFER_SIZE` has
  overflowed). It latches per slot — verdict, name, uptime, the cpu/wall that tripped it, whether
  params were reset, and current state — **until explicitly cleared**, and it reports slots that
  have since recovered, which is the point: a transient fault looks healthy in `ext list`.
  Discovery-time failures latch too, so an empty report is real evidence.
- **A CPU fault also latches its crash site**: `k_sys_fatal_error_handler` hands the reason +
  exception frame to `extension_host::noteSandboxFault()`, which resolves PC and LR against the
  resident llext's `mem[]`/`mem_size[]` into **section-relative offsets** (the dependency-free
  `extension_fault_pc.h`, suite `extensions.fault_pc`) and parks them in `sPendingCrash` for the next
  `sandbox_fault()` to fold into the record. `ext faults` then prints `pc 0x... = .text+0x<off>` plus
  a paste-ready `arm-none-eabi-addr2line -f -j .text -e <file>.llext.debug 0x<off>` line for the
  release's debug sidecar (PR #429). Two non-obvious points: the handler runs in exception context
  (no lock, no log, no allocation) and reads a dedicated `sCrashExt` pointer, because
  `runtime_load()` starts the sandbox thread *before* `sResident.ext` is assigned and `rgbx_init`
  can fault in that window; and the verdict order (CPU overrun → Completed → SandboxDied) means a
  crashed thread can be reported as `CpuBudgetExceeded` — the crash site is attached whenever one is
  pending, regardless of verdict. Verified on proto0 2026-09-02: `hello` with `Crash` set reports
  `pc = .text+0x11c`, which a `-g` build resolves to `hello.c:137` (the deliberate kernel-SRAM
  write), while the LR is *outside* the extension — it resolves in `zephyr.elf` to the host's
  `sandbox_entry`, the normal shape for a crash directly in `rgbx_tick`. Reader-facing recipe:
  `fw/extensions/README.md` "Resolving a crash to a source line".

## Tick budget and fault recovery

- **The per-tick budget is CPU time, never wall time (issue #276).**
  `CONFIG_APP_EXT_TICK_CPU_BUDGET_MS` is charged against the sandbox thread's own
  `execution_cycles` (hence `APP_EXTENSION_HOST select THREAD_RUNTIME_STATS`);
  `CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS` is only a ceiling for an extension that *blocks* rather
  than spins. Both handshake sites are covered — the steady-state tick and the `rgbx_init` wait in
  `runtime_load()` — sharing one deadline per `tick()`. Decision logic is the dependency-free
  `fw/src/extensions/extension_tick_budget.h`, covered by `extensions.tick_budget`. **If you
  reintroduce a wall-clock deadline in this path you reintroduce the bug.** Rationale, measured
  numbers, the lock-hold trade-off and exact bounds live in `fw/docs/threading.md` — the single
  system-wide map; don't restate them elsewhere.
- **Every tick-time fault clears the slot's persisted params; load/init-time failures do not.**
  Params reach the extension only at tick time, so an init failure can't have been caused by them.
  Do not try to spare the blocked/wall-backstop case: an extension that burns no CPU is often
  blocked *because* a parameter sent it down a waiting path, and sparing it leaves the slot unable
  to recover across `ext select` or a reboot.
- **Fault recovery is deliberate**: a dead sandbox is unloaded, un-marks + notifies the animation's
  Is Active characteristic (app toggle turns off), scrolls a `FAULT: <name>` banner on the panel
  (proxy), and BLE re-activation is rejected; only `ext select <slot>` clears the fault and retries.
  The host serializes activate/deactivate/tick/param-writes with a mutex because
  `pattern_controller_change_to_animation()` runs on the *caller's* thread.
