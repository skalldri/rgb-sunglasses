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
never preempted. Not by a preemptible thread, and not by a higher-priority cooperative
thread either. It runs until it blocks, sleeps, or yields. A cooperative thread's priority
number only decides which cooperative thread is picked next once the running one lets go.

Two consequences that drive most of the layout below:

- No priority you can assign in the preemptible band will let a thread run while a
  cooperative thread is executing. If a preemptible thread is being starved by BT or by
  the system workqueue, changing its number cannot fix that.
- A thread that does filesystem or flash I/O must stay preemptible. A long flash write
  from a cooperative thread starves the entire system (this is a standing rule in
  `fw/CLAUDE.md`, from PR #51).

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
| −7 | **app** `audio_dsp_thread` | coop | `CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY` |
| −6 | `bmi270_thread` (IMU driver's own trigger thread) | coop | `CONFIG_BMI270_THREAD_PRIORITY` (=10, wrapped in `K_PRIO_COOP`) |
| −1 | `sysworkq` | coop | `CONFIG_SYSTEM_WORKQUEUE_PRIORITY` |
| −1 | `usbd_msc` | coop | USB mass-storage |
| 0 | `main`, `mbox_wq #0` | preempt | `CONFIG_MAIN_THREAD_PRIORITY` |
| 3 | `mcumgr smp` | preempt | mcumgr transport |
| 5 | **app** `tps25750_wq` (PD/charger driver) | preempt | `CONFIG_TPS25750_WORKQ_PRIORITY` |
| 6 | **app** `led_display_thread` | preempt | `CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY` |
| 6 | **app** `pattern_controller_thread` | preempt | `CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY` |
| 6 | **app** `bt_thread` (application state machine) | preempt | `CONFIG_APP_BT_THREAD_PRIORITY` |
| 7 | **app** `imu_thread` | preempt | `CONFIG_IMU_THREAD_PRIORITY` |
| 7 | **app** `status_led_thread` | preempt | `CONFIG_APP_STATUS_LED_THREAD_PRIORITY` |
| 7 | **app** `charger_status_thread` | preempt | `CONFIG_APP_CHARGER_STATUS_THREAD_PRIORITY` |
| 7 | **app** extension sandbox thread | preempt | `CONFIG_APP_EXT_HOST_THREAD_PRIORITY` |
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
| `APP_EXT_HOST_THREAD_PRIORITY > APP_PATTERN_CONTROLLER_THREAD_PRIORITY` | `src/extensions/extension_host.cpp` | The pattern controller enforces the extension's per-tick deadline; it must be able to preempt a runaway sandbox. |
| `IMU_THREAD_PRIORITY > APP_PATTERN_CONTROLLER_THREAD_PRIORITY` | `src/imu/imu.cpp` | The animation tick must preempt the sensor reader. |
| The three flash/FS workqueue priorities are valid preemptible values | their respective `.cpp` files | They do the longest blocking I/O in the system and must stay at the bottom. |

Not machine-checkable, but equally binding: **the TPS25750 workqueue must rank below the
rendering threads.** It runs multi-step CMD1/DATA1 bridge transactions under the driver's
task mutex and can hold the CPU for a while. It cannot be asserted in the driver, which is
built standalone in two test suites and does not see the application's symbols.

## Known contention

Two structural problems are visible in the map above and are tracked by
[issue #267](https://github.com/skalldri/rgb-sunglasses/issues/267):

1. `led_display_thread`, `pattern_controller_thread` and `bt_thread` all share priority 6.
   The frame-deadline thread round-robins at 20 ms granularity against two threads that do
   long blocking work.
2. The TPS25750 workqueue at priority 5 outranks every rendering thread.

Separately, everything in the cooperative band — the BT host/controller threads, the
system workqueue (which is where BT bond and CCC records get written to NVS/QSPI),
`bmi270_thread`, and `audio_dsp_thread` — can block rendering for as long as it runs, and
no preemptible priority can change that. Note that `audio_dsp_thread` and `bmi270_thread`
are ours to move: both are configured from symbols we own.

The BT long workqueue is *not* part of that set, despite the name pattern — it is
preemptible at 10 (see above).

Note that this is a CPU-scheduling problem, not bus contention: the WS2812 strips are on
SPI1/SPI2/SPI4, the MX25R6435F is on QSPI, and the BMI270 is on SPI3, so flash traffic and
LED output never contend for a peripheral.

## Measuring

`CONFIG_THREAD_RUNTIME_STATS`, `CONFIG_THREAD_MONITOR`, `CONFIG_THREAD_NAME` and
`CONFIG_KERNEL_SHELL` are all enabled, so `kernel threads` on the serial shell reports every
thread's priority, state, stack high-water mark and cumulative runtime. That is the first
tool to reach for when a priority change needs to be justified or verified — capture it
before and after.
