# Thread priorities and stack sizes

This is the single system-wide map of every thread in the appcore image, what governs its
priority, and the invariants that must hold between them. Read it before changing any
thread priority — several of the relationships below are not derivable from the code at
any one call site.

## Zephyr's priority convention

```
-CONFIG_NUM_COOP_PRIORITIES  ..  -1     cooperative
 0  ..  CONFIG_NUM_PREEMPT_PRIORITIES-1  preemptible
```

Lower number = higher priority, in both bands. On proto0 today
`CONFIG_NUM_COOP_PRIORITIES=16` and `CONFIG_NUM_PREEMPT_PRIORITIES=15`, so the legal range
is `-16 .. 14`.

**The critical property, and the one that surprises people:** a *cooperative* thread is
never preempted. Not by a preemptible thread, and **not by a higher-priority cooperative
thread either**. It runs until it blocks, sleeps, or yields. A cooperative thread's
priority number only decides which cooperative thread is picked next once the running one
lets go.

This is not folklore — it is `should_preempt()` in
`zephyr/kernel/include/kthread.h`, which returns false unless the **currently running**
thread is preemptible (or the incoming thread is a meta-IRQ, which nothing here is).

Three consequences that drive the whole layout below:

- No priority you can assign in the preemptible band will let a thread run while a
  cooperative thread is executing. If a preemptible thread is being starved by a
  cooperative one, changing its number cannot fix that.
- **Nor can promoting the starved thread into the cooperative band.** That is the trap
  issue #267 was originally going to walk into: making `led_display_thread` cooperative
  would not have let it preempt the cooperative Bluetooth or system-workqueue threads
  blocking it. It would only have stopped the BLE radio threads from preempting the
  display thread — strictly worse. **The fix for a cooperative blocker is to move the
  blocker out of the cooperative band, not to join it.**
- A thread that does filesystem or flash I/O must stay preemptible. A long flash write
  from a cooperative thread starves the entire system (this is a standing rule in
  `fw/CLAUDE.md`, from PR #51). The system workqueue is a live example — see below.

Timeslicing is on: `CONFIG_TIMESLICE_SIZE=20`, `CONFIG_TIMESLICE_PRIORITY=0`. Threads that
share a preemptible priority therefore round-robin at 20 ms granularity — which is coarse
relative to the 33.3 ms display period, so sharing a priority with the display thread costs
real frame jitter.

## The map

Sorted by effective priority, highest first. This table was cross-checked against a real
`kernel thread list` capture on proto0 — do the same before trusting it after any change.
Rows marked **app** are ours; the rest come from Zephyr/NCS and are tuned from `prj.conf`.

| Prio | Thread | Band | Kconfig symbol |
| ---: | --- | --- | --- |
| −10 | BT controller RX (high) | coop | `CONFIG_BT_DRIVER_RX_HIGH_PRIO` (=6, wrapped in `K_PRIO_COOP`) |
| −9 | BT HCI TX | coop | `CONFIG_BT_HCI_TX_PRIO` (=7) |
| −8 | `BT RX WQ` | coop | `CONFIG_BT_RX_PRIO` (=8) |
| −8 | `usbd`, `udc_nrfx` | coop | USB device-next stack |
| −6 | `bmi270_thread` (IMU driver's own trigger thread) | coop | `CONFIG_BMI270_THREAD_PRIORITY` (=10, wrapped in `K_PRIO_COOP`) |
| −1 | `sysworkq` | coop | `CONFIG_SYSTEM_WORKQUEUE_PRIORITY` — SDK-pinned to the coop band, see below |
| −1 | `usbd_msc` | coop | USB mass-storage |
| 0 | `main`, `mbox_wq #0` | preempt | `CONFIG_MAIN_THREAD_PRIORITY` |
| 2 | **app** `led_display_thread` | preempt | `CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY` |
| 3 | `mcumgr smp` | preempt | `CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO` |
| 4 | **app** `pattern_controller_thread` | preempt | `CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY` |
| 5 | **app** `audio_dsp_thread` | preempt | `CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY` |
| 6 | **app** `bt_thread` (application state machine) | preempt | `CONFIG_APP_BT_THREAD_PRIORITY` |
| 7 | **app** `imu_thread` | preempt | `CONFIG_IMU_THREAD_PRIORITY` |
| 8 | **app** `status_led_thread` | preempt | `CONFIG_APP_STATUS_LED_THREAD_PRIORITY` |
| 8 | **app** `charger_status_thread` | preempt | `CONFIG_APP_CHARGER_STATUS_THREAD_PRIORITY` |
| 9 | **app** extension sandbox thread | preempt | `CONFIG_APP_EXT_HOST_THREAD_PRIORITY` |
| 10 | **app** `tps25750_wq` (PD/charger driver) | preempt | `CONFIG_TPS25750_WORKQ_PRIORITY` |
| 10 | `BT LW WQ` (ECDH / pairing crypto) | preempt | `CONFIG_BT_LONG_WQ_PRIO` |
| 14 | **app** `persist_wq` | preempt | `CONFIG_APP_PERSIST_WORKQ_PRIORITY` |
| 14 | **app** `coredump_wq` | preempt | `CONFIG_APP_COREDUMP_WORKQ_PRIORITY` |
| 14 | **app** `mcuboot_upd_wq` | preempt | `CONFIG_APP_MCUBOOT_UPDATER_WORKQ_PRIORITY` |
| 14 | `shell_uart`, `logging` | preempt | Zephyr |
| 15 | `idle` | preempt | Zephyr |

Also configurable, not application threads: `CONFIG_SHELL_STACK_SIZE`,
`CONFIG_MAIN_STACK_SIZE`, `CONFIG_ISR_STACK_SIZE`, `CONFIG_BT_RX_STACK_SIZE`,
`CONFIG_BT_LONG_WQ_STACK_SIZE`, `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE`,
`CONFIG_UDC_NRF_THREAD_STACK_SIZE` — all set in `fw/prj.conf` or the proto0 board `.conf`.

### Which "priority" symbols are cooperative

Do not assume from the number. Each of these is wrapped in `K_PRIO_COOP()` at the call
site, so a positive Kconfig value produces a *negative* (cooperative) runtime priority:

- `CONFIG_BT_RX_PRIO`, `CONFIG_BT_DRIVER_RX_HIGH_PRIO`
  (`zephyr/subsys/bluetooth/host/hci_core.c:4408`, `.../controller/hci/hci_driver.c:1027,1034`)
- `CONFIG_BMI270_THREAD_PRIORITY`
  (`zephyr/drivers/sensor/bosch/bmi270/bmi270_trigger.c:182`)

Raising those numbers moves the thread *within* the cooperative band; it can never make
them preemptible.

`CONFIG_BT_LONG_WQ_PRIO` is the counter-example — `zephyr/subsys/bluetooth/host/long_wq.c:42`
passes it through **unwrapped**, so its default of 10 really is preemptible priority 10,
below all our rendering threads. (This is easy to get wrong by analogy with `BT_RX_PRIO`;
the on-device capture is what settles it.)

## Stack sizes

| Thread | Kconfig symbol | Default |
| --- | --- | ---: |
| `led_display_thread` | `CONFIG_APP_LED_DISPLAY_THREAD_STACK_SIZE` | 4096 |
| `pattern_controller_thread` | `CONFIG_APP_PATTERN_CONTROLLER_THREAD_STACK_SIZE` | 4096 |
| `bt_thread` | `CONFIG_APP_BT_THREAD_STACK_SIZE` | 2048 |
| `status_led_thread` | `CONFIG_APP_STATUS_LED_THREAD_STACK_SIZE` | 2048 |
| `audio_dsp_thread` | `CONFIG_APP_AUDIO_DSP_THREAD_STACK_SIZE` | 2048 |
| `imu_thread` | `CONFIG_IMU_THREAD_STACK_SIZE` | 1024 |
| `charger_status_thread` | `CONFIG_APP_CHARGER_STATUS_THREAD_STACK_SIZE` | 1024 |
| extension sandbox | `CONFIG_APP_EXT_HOST_STACK_SIZE` | 2048 |
| persistent-value-store wq | `CONFIG_APP_PERSIST_WORKQ_STACK_SIZE` | 2048 |
| `mcumgr smp` workqueue | `CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE` | **4096 on proto0** (Zephyr default 2048) |

**The SMP workqueue's 4096 is an invariant, not headroom to reclaim.** The
FILE_MGMT DELETE handler (`extension_mgmt.cpp`, PR #303) runs the full
animation-switch path on this thread when the deleted extension backs the
current animation — `pattern_controller_change_to_animation` → deactivate /
`unload_resident` (thread abort + llext teardown) → `bt_gatt_notify` — a
callee set that had only ever run on the BT RX (4096) or shell (6656) stacks.
Measured on hardware at the deepest path (delete of the faulted-active
extension, live BLE subscription): **1,672 B peak**, which would have left
only ~376 B of the old 2,048 B default — the same stack a 2,048-byte
`fs_mgmt` chunk array once overflowed with a board-resetting crash (see the
`DL_CHUNK_SIZE` comment in the proto0 conf). Set in
`fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`.
| coredump manager wq | `CONFIG_APP_COREDUMP_WORKQ_STACK_SIZE` | 3072 |
| MCUboot updater wq | `CONFIG_APP_MCUBOOT_UPDATER_STACK_SIZE` | 4096 |
| TPS25750 wq | `CONFIG_TPS25750_WORKQ_STACK_SIZE` | 1024 |

Most application threads use `K_KERNEL_THREAD_DEFINE` / `K_KERNEL_STACK_DEFINE`, which
skips the 1 KB `CONFIG_USERSPACE` privileged-stack reservation. Those stacks can never host
a `K_USER` thread. The two exceptions are `imu_thread` and the extension sandbox, both of
which *are* user-mode threads and so use `K_THREAD_STACK_DEFINE`.

Verify stack sizing against the real high-water marks (`kernel thread list` on the serial
shell), not against guesses — the 2048 B figures for `bt_thread` and `audio_dsp_thread`
came from measured marks of 724 B and 692 B respectively during issue #75.

**`charger_status_thread` is the one to watch: 912 B of 1024 B (89%) in the proto0
capture.** Everything else measured at or below 45%. Its call path goes through the
TPS25750 I2Cm bridge into the BQ25792, so anything added there eats directly into that
112 B of remaining headroom.

## Invariants

Each of these is enforced by a `BUILD_ASSERT` next to the thread it constrains, so a bad
`prj.conf` override fails the build rather than misbehaving at runtime.

| Invariant | Asserted in | Why |
| --- | --- | --- |
| `APP_LED_DISPLAY_THREAD_PRIORITY <= APP_PATTERN_CONTROLLER_THREAD_PRIORITY` | `src/led_controller.cpp` | The display thread owns the only hard frame deadline; it must never rank below its own producer. |
| `APP_PATTERN_CONTROLLER_THREAD_PRIORITY >= 0` | `src/pattern_controller.cpp` | It does FAT/QSPI I/O (GLIM assets, `.llext` loads) and settings persistence. |
| `APP_EXT_HOST_THREAD_PRIORITY > APP_PATTERN_CONTROLLER_THREAD_PRIORITY` | `src/extensions/extension_host.cpp` | The pattern controller enforces the extension's per-tick budget; it must be able to preempt a runaway sandbox. |
| `APP_EXT_TICK_WALL_BACKSTOP_MS > APP_EXT_TICK_CPU_BUDGET_MS` | `src/extensions/extension_host.cpp` | A tick cannot consume more CPU than wall time, so an inverted pair would make the backstop fire first and reintroduce the issue #276 false positives. |
| `IMU_THREAD_PRIORITY > APP_PATTERN_CONTROLLER_THREAD_PRIORITY` | `src/imu/imu.cpp` | The animation tick must preempt the sensor reader. |
| The three flash/FS workqueue priorities are valid preemptible values | their respective `.cpp` files | They do the longest blocking I/O in the system and must stay at the bottom. |

Not machine-checkable, but equally binding: **the TPS25750 workqueue must rank below the
rendering threads.** It runs multi-step CMD1/DATA1 bridge transactions under the driver's
task mutex and can hold the CPU for a while. It cannot be asserted in the driver, which is
built standalone in two test suites and does not see the application's symbols.

### The extension sandbox's scheduling latency is unbounded — by design

The assert above is the *only* schedulability property of the sandbox that a build can
check. It says the deadline enforcer can preempt a runaway extension. It deliberately says
nothing about how long the sandbox may go **unscheduled**, and nothing can: the sandbox
sits at the bottom of the application band, so its worst-case scheduling latency is the
summed worst-case work of every numerically-lower-priority thread — today `led_display`
(2), `mcumgr smp` (3), `pattern_controller` (4), `audio_dsp` (5), `bt_thread` (6),
`imu_thread` (7), `status_led`/`charger` (8), plus the whole cooperative band above them.
That figure is not expressible in Kconfig and changes with every retune.

**This is why the per-tick budget is CPU time, not wall time (issue #276).** The host used
to time the handshake with a wall-clock `k_sem_take` timeout, which silently made
correctness depend on the latency bound above. It broke exactly as you would expect: after
#271 moved the sandbox from 7 to 9 and `led_display` from 6 to 2, a single 41.5 ms display
frame (measured — `led_stats`, `work max`) consumed 83% of the 50 ms deadline, and a
healthy extension whose own cost was 4.7 ms started faulting. `ext stats` now prints CPU
and wall separately so the two can never be conflated again.

The rule for future retunes: **you may move the sandbox anywhere the assert allows without
worrying about extension false-positives**, because nothing in the fault path measures
elapsed time any more except the deliberately-generous `APP_EXT_TICK_WALL_BACKSTOP_MS`. If
you ever reintroduce a wall-clock deadline here, you reintroduce this bug.

**The trade-off that buys, and who pays it.** `extension_host`'s mutex is held across the
whole handshake, so its worst-case hold time is the handshake's worst case — and CPU
budgeting deliberately lengthened that. The old wall deadline capped the hold at 50 ms by
declaring a starved tick dead; now a starved-but-healthy tick is waited out, and only a
genuinely *blocked* extension runs to the backstop. Concurrent BLE parameter writes and
`ext select` share that mutex and block for the same duration, on any slot. Measured on
proto0 with the companion app connected, worst handshake wall time was **8.3 ms** — the
backstop is reached only in the fault case, once, immediately before the extension is
unloaded. So the recurring cost tracks real scheduling latency, not the backstop; but if
you raise `APP_EXT_TICK_WALL_BACKSTOP_MS`, you are widening a BLE-visible stall, not just a
fault threshold.

Two bounds that are easy to get wrong, both load-bearing:

- **The ceiling is `APP_EXT_TICK_WALL_BACKSTOP_MS + one poll period`**, not the backstop
  alone — the deadline is only re-checked when a poll expires. ~510 ms at the defaults.
  Size host-side timeouts against the sum.
- **One `tick()` computes one deadline and shares it** with the lazy load's `rgbx_init`
  handshake. Without that, a tick that triggers a load would run two full handshakes under
  one lock acquisition and could hold it for twice the advertised ceiling.

**Detection latency is load-dependent, and that is correct.** A CPU budget is spent at
whatever rate the scheduler grants, so an extension given 10% of the CPU needs ~10x the
wall time to burn 50 ms of CPU. A runaway is therefore caught later on a loaded system than
the old flat wall deadline caught it; the wall backstop is what bounds that tail. Do not
"fix" this by reintroducing a wall deadline — that is the #276 bug.

## What issue #267 changed, and why

The reported symptom was animations freezing for a visible 100–500 ms during Bluetooth and
flash activity. Four distinct causes, all visible in the map above before the fix:

1. **`led_display_thread`, `pattern_controller_thread` and `bt_thread` all shared priority
   6.** The one thread with a frame deadline round-robined at the 20 ms timeslice against
   two threads that do long blocking work. → display 3, pattern controller 4, bt_thread 6.
2. **`audio_dsp_thread` was cooperative at −7.** A CMSIS-DSP FFT therefore stalled every
   rendering thread for its full duration, unpreemptably. → preemptible 5.
3. **The TPS25750 workqueue at 5 outranked every rendering thread**, and its handlers run
   multi-step I2C bridge transactions under a mutex. → 10.

The tempting fourth "fix" — promoting `led_display_thread` into the cooperative band — is
wrong, for the reason given at the top of this document: it would not have let it preempt
causes 2–3, only stopped the radio from preempting it.

**Do not put the display thread at priority 3.** That is where Zephyr fixes the mcumgr SMP
transport workqueue (`CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO`), and the app holds an
SMP link open. An intermediate version of this change used 3 and measured a full dropped
frame because of it — worst wake-to-wake went 43.2 ms → 63.8 ms, reproducibly across two
runs, purely from the 20 ms timeslice round-robin between the two. Priority 2 is free and
sits above it. This is the whole reason the map above lists non-application threads too:
picking a number without checking who else is already on it just moves the collision.

### What is still unpreemptable

Three things remain cooperative and can still block rendering for as long as they run.

**`sysworkq` (−1) is the significant one, and it cannot be fixed from here.** The
Bluetooth host queues its bond/CCC/settings records onto the system workqueue
(`subsys/bluetooth/host/settings.c`), so those become NVS writes to the external QSPI
flash from a thread nothing can preempt. Raising `CONFIG_SYSTEM_WORKQUEUE_PRIORITY` into
the preemptible band was tried for issue #267 and is **rejected at configure time**: both
`zephyr/subsys/bluetooth/Kconfig` ("The Bluetooth subsystem requires the system workqueue
to execute at a cooperative priority") and
`zephyr/subsys/ipc/ipc_service/lib/Kconfig.icmsg` constrain the symbol to `range -256 -1`,
and the SDK is not ours to modify. What limits the damage in practice is that these writes
are event-driven (pairing, CCC subscription changes) rather than continuous, and that
`CONFIG_BT_SETTINGS_DELAYED_STORE_MS=1000` already batches them. **The project's own
persisted config does not go through here** — it has its own preemptible workqueue at 14
(`persist_wq`), which is exactly why that indirection exists.

The BT host/controller threads (−10/−9/−8) and `bmi270_thread` (−6) also remain
cooperative, but each does short, bounded per-invocation work (one HCI packet, one sensor
trigger) rather than an FFT or a flash erase, so they are accepted as-is. `BT LW WQ` is
*not* in this set despite the name pattern — it is preemptible at 10.

Note this was a CPU-scheduling problem throughout, not bus contention: the WS2812 strips
are on SPI1/SPI2/SPI4, the MX25R6435F is on QSPI, and the BMI270 is on SPI3, so flash
traffic and LED output never contend for a peripheral.

### Measuring the symptom

`led_stats` on the serial shell reports frame **wake-to-wake interval** (min/avg/max),
late frames (>2× target), worst per-frame work, and the worst segment between yield
points. `led_stats reset` zeroes it. Interval is the metric that corresponds to the
user-visible stutter — the pre-existing `LOG_WRN` only measured the display thread's own
work time, which stays perfectly healthy while another thread is hogging the CPU, so it
could not have caught any of the four causes above.

## Measuring

`CONFIG_THREAD_RUNTIME_STATS`, `CONFIG_THREAD_MONITOR`, `CONFIG_THREAD_NAME` and
`CONFIG_KERNEL_SHELL` are all enabled, so `kernel threads` on the serial shell reports every
thread's priority, state, stack high-water mark and cumulative runtime. That is the first
tool to reach for when a priority change needs to be justified or verified — capture it
before and after.
