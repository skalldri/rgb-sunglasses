---
paths:
  - "fw/src/power.*"
  - "fw/src/power/**"
  - "fw/src/battery_*.h"
  - "fw/src/bluetooth/power_debug_service.*"
  - "fw/drivers/tps25750/**"
  - "fw/drivers/bq25792/**"
  - "fw/drivers/emul_tps25750/**"
  - "fw/tests/power/**"
  - "fw/tests/drivers/emul_tps25750/**"
  - "fw/tests/drivers/bq25792_decode/**"
  - "fw/dts/bindings/charger/**"
  - "fw/dts/bindings/usb-c/**"
---

# Power subsystem: TPS25750 PD controller + BQ25792 charger

Loads when you read power code. **Every write to these parts is governed by root `CLAUDE.md`
"NEVER write unverified commands or data into hardware parts"**: the TRM/datasheet in hand first
(`fw/docs/datasheets/`), or stop and ask. Symptom playbook: `/debug-fw`.

`fw/src/power.cpp` / `fw/drivers/` — TPS25750 USB PD controller (custom driver, patch loaded via an
LZ4-compressed blob) and BQ25792 battery charger (custom driver), both I2C.

## Power subsystem: safe vs danger

All `power` shell commands are registered in `fw/src/power.cpp`.

- **SAFE (read-only)**: `power bq status`, `power bq limits` (ICHG/IINDPM/VINDPM/ICO/watchdog
  readbacks + DPM status flags — the first stop for "charging too slow"), `power bq dump`,
  `power pd dump`, `power pd contract` (negotiated PD contract / Type-C budget + advertised sink
  caps), `power policy` (charger-policy state), `power vreghvout`, and the `bq25792_get_*` driver
  getters. All `bq25792_get_*` getters propagate I2C errors (negative errno, output untouched) — the
  legacy ADC/status getters used to swallow bus errors and return stale/zero-but-plausible data,
  which hid I2Cm bridge outages (fixed alongside the PTCH-wedge recovery work; callers that key on
  the return value, like the charger status thread's `vbat_ok`/`chg_ok`, rely on this).
- **DANGER (writes)**: any register write, 4CC task, or patch operation — `power pd patch ...`
  (`tps25750_download_patch()`), `power pd clear_dbfg`, `power pd go2p`, the `power bq
  charge`/`adc`/`pfm`/`freq`/`temp_override`/`hiz`/`ichg` setters (`bq25792_set_*`), and
  **especially `power boost`**, which writes UICR `VREGHVOUT` — irreversible without a mass chip
  erase.
- **GO2P** (`tps25750_go2p()`, user-commissioned 2026-07-17, cited to TRM SLVUC05A Table 3-12) is a
  TRM-cited task that forces PTCH mode to exercise the runtime PTCH-wedge recovery path. It refuses
  to run without a battery present, and hardware-tested 2026-07-17 it is **cleanly REJECTED on
  proto0** with PatchConfigSource=6 per TRM Table 3-12 — see `tps25750_go2p()`. The incident that
  made this rule: `docs/agent-incidents.md#2026-07-05-tps25750-go2p-wedge`.

## Wrong/implausible BQ25792 current or voltage

**There are two known in-repo root-cause classes — rule both out (`/debug-fw`'s device-symptoms
table) before pursuing any external fix or register write:**

1. Missing two's-complement sign extension in the ADC decode (IBAT/IBUS are 16-bit signed; fixed in
   PR #106, regression suite `fw/tests/drivers/bq25792_decode`).
2. Interleaved I2Cm bridge transactions (fixed in PR #111 with the `task_mutex`). Interleaving
   corruption shows up as **plausible-but-wrong values** (e.g. VBAT read back as the VBUS value),
   not as I2C errors.

**Every BQ25792 register access goes through the TPS25750 I2Cm bridge** — the bq25792 DT node is a
child of the tps25750 node — via its CMD1/DATA1 4CC sequence under that `task_mutex`
(`fw/drivers/tps25750/tps25750.c`). Any new BQ25792 write inherits that path and its serialization
requirements (`fw/CLAUDE.md` "Coding rules": per-device `k_mutex`, `_locked` inner functions,
bounded poll loops).

## Prefer native_sim first

`fw/tests/drivers/emul_tps25750` runs the **real** tps25750 + bq25792 drivers against an emulated
register file — no hardware, no risk. Emulator mechanics (second I2C address for patch chunks,
nonzero CMD1 busy window, `DTS_ROOT`): `.claude/rules/fw-tests.md`.
