# Proto0 Power Management Overhaul — Investigation Results + Plan

> **Step 0 of implementation**: commit this document as `docs/plans/power-management-overhaul.md` (user wants it checked in; plan mode couldn't write to the repo). Update it as hardware findings land.

## Context

Proto0's power situation is worse than expected: the faceplate pulls 1.2–1.8W even idle against a nominal 2.5W standard-USB budget. Five symptoms/asks from bench testing:

1. **Reboot loop** with no batteries + charging enabled (charging disabled → boots fine).
2. **~20mA battery charging** with the faceplate connected.
3. **~200mA fast-charge cap** even with faceplate disconnected; make charge rate configurable (**default 900mA** per user).
4. **Expose battery/PD debug info to the app**; main page shows only charge status + percentage, tap-through to a detail/settings page (like Core Config).
5. **Charging LED indication colored by estimated charge percentage** (discrete colors, onboard status LED — user confirmed).

Power tree: USB-C → TPS25750 (PD controller) → BQ25792 (buck-boost charger) → VSYS → 5V buck → faceplate (1.2W+ idle) and → 3.3V buck → nRF5340 (~0.6W). 2S LiPo (~8.4V) on BAT. All BQ25792 register access is bridged through the TPS25750's I2Cm port (CMD1/DATA1 4CC tasks under `task_mutex`).

Datasheets are checked in under `fw/docs/datasheets/` (BQ25792 SLUSDG1C; TPS25750 SLVSFR7A; host-interface TRM SLVUC05A). Every register write below carries a datasheet citation per the project hardware-safety rule.

## Investigation results (datasheets + code, pre-hardware)

### Symptom 1 — reboot loop: documented BQ25792 behavior (high confidence)

BQ25792 §9.3.6 (PDF p.34): with **charge enabled and no battery**, the charger "continuously tries to charge the BAT capacitance, typically resulting in the BAT voltage alternating between a low level and BATOVP fault. **The SYS voltage follows the battery voltage up**, potentially reaching SYSOVP." SYS feeds our bucks → nRF5340 browns out → reboots → firmware blindly re-applies the persisted charge-enable (default ON) in `battery_service_apply_boot_state()` → loop. With charge **disabled**, same section: SYS regulates cleanly at VSYSMIN — matches observation exactly.

**Fix (user-approved): auto-gate charge enable on battery presence** — `VBAT_PRESENT_STAT` REG1D[0] (p.85) — at boot *and* runtime (removal while charging). The persisted user toggle is gated, never overwritten.

### Symptoms 2+3 — one root cause: input power budget (and who programs the charger)

- Our BQ25792 driver writes **zero** charge-config registers. But the flashed TPS25750 patch bundle (`fw/misc/tps25750/tps25750-config.c`; ACT questionnaire `TPS25750-config.json`: battery 8.4V, **charge current 0.5A**, precharge 0.12A, termination 0.04A) is built with TI's **BQ charger integration** — the TPS25750 itself programs the BQ25792 over the same I2Cm port on PD events. (Answers the user's question: yes, that's why BQ hangs off the TPS's I2Cm port. An unused `tps25750-no-bq.c` variant also exists.)
- NVDC power path (§9.3.8 p.37, §9.3.9.2 p.40): **system load has absolute priority**; charge current ≈ input budget − system load. At 5V/500mA: no faceplate → ~1.9W spare ≈ 200mA into ~8.2V pack (**observed**); faceplate on → ~0 spare (**observed 20mA**).
- Which limit binds is directly readable: `IINDPM_STAT`/`VINDPM_STAT` REG1B[7]/[6] (p.81). IINDPM (REG06, 10mA/LSB) is auto-set by BC1.2 detection (SDP=500mA) and/or the TPS bundle; VINDPM (REG05) POR=3.6V catches sagging sources.
- **BQ I2C watchdog** (REG10[2:0], POR **40s**, p.68) is never fed/disabled by us. On expiry, watchdog-scoped registers revert to POR (ICHG→2A, EN_CHG→enabled). TPS-bundle writes incidentally reset the timer, so real on-board state over time is unknown until measured (`WD_STAT` REG1B[5]).
- TRM p.60: TPS event-driven charger writes and host `I2Cw` writes **share one queue**; TI mandates read-back verification of host writes.

### Latent bugs found during design review / PR A implementation (all fixed in PR A)

- `fw/include/zephyr/drivers/InternalDeviceRegister.hpp` — `read()` byte-swaps 16/32-bit registers from the BQ's big-endian wire format, but `flush()` (literal `// TODO: probably also need endian-swapping logic here`) wrote host-order raw. Dormant because every existing write was 8-bit; **ICHG (REG03) and IINDPM (REG06) are 16-bit** — writing them through the unfixed path would have programmed byte-swapped garbage. Covered by a 16-bit round-trip regression test against the (wire-order) emulator register file.
- `fw/drivers/bq25792/bq25792_priv.h` — `BC1_2_DONE_STAT` was declared at REG1C bit 1 (aliasing VBUS_STAT's LSB); datasheet Table 9-37 puts it at bit 0.
- `fw/drivers/bq25792/bq25792_priv.h` — `WATCHDOG` was declared 2 bits wide; Table 9-26 says WATCHDOG_2:0 = bits 2:0 (3 bits). A "disable" (write 0) through the 2-bit field would have left bit 2 set — **watchdog still armed at the 20s setting**, silently reverting charger config. Caught by the new emulator tests.

## Hardware experiments (designed first, per user request — run before behavior fixes)

**Experiment A — input-limit diagnosis (read-only, safe).** Flash PR-A image (new *read-only* shell commands). With battery installed, for each source (PC port; a PD charger): capture `power bq limits` + `power pd contract` every ~10s for ~2min, with and without faceplate. Answers: (1) IINDPM vs VINDPM — which DPM flag is active during the 20mA symptom; (2) actual ICHG/IINDPM values over time — is the TPS bundle writing them, and does the watchdog expire (`WD_STAT`) and revert them; (3) the negotiated contract/Type-C tier; (4) TX_SINK_CAPS (0x33) — what the ACT bundle advertises as sink, i.e. whether >5V contracts are even negotiable today or the bundle needs rebuilding; (5) VAC_OVP readback (REG10[5:4]) to confirm the input-OVP headroom for >5V contracts. No writes to either chip beyond what today's firmware already does.

**Experiment B — reboot-loop confirmation (optional, scope traces for the record).** No battery, charge toggle off → boot, confirm `VBAT_PRESENT=0` → enable charging while scoping SYS + VBAT: expect BAT ramp/BATOVP cycling, SYS following (§9.3.6 case 2). The fix doesn't depend on this; skip unless the traces are wanted.

## Experiment A — first hardware data (2026-07-12, PR A image, bench charger, no faceplate, battery ~full)

From `power bq limits` / `power pd contract` on the freshly-flashed PR A image:

- **`ICHG=500 mA` confirmed on silicon** — the TPS25750 bundle programs the BQ25792 charge current to the ACT questionnaire's 0.5A. This is symptom 3's cap (input budget permitting).
- **`WATCHDOG=disabled`** — the TPS bundle itself disables the BQ I2C watchdog. Removes the config-revert hazard from the picture and simplifies PR B (the policy's watchdog-disable becomes a reconcile-verified invariant, not a fight).
- **`VAC_OVP=26V`** — the datasheet's self-inconsistent REG10 reset value resolves to the field-table POR (00b = 26V) on real silicon; no OVP obstacle to >5V contracts.
- **`VINDPM=4600 mV`, `IINDPM=3000 mA`** — also bundle-programmed (not the 3600/BC1.2 PORs).
- **The bundle advertises exactly ONE sink PDO: fixed 5V/3A** (`TX_SINK_CAPS`), and with a PD charger it lands an explicit **5V @ 3A (15W)** contract (`ACTIVE_CONTRACT_PDO=0x1d11912c`). Confirms: >5V contracts are impossible without an ACT bundle rebuild — required for the variable-voltage plan in PR C.
- Battery was ~full during this capture (VBAT 8.35V, taper, IBAT≈0) — the 20mA/200mA reproduction against a weak (500mA host) source is still to be captured with a drained pack + faceplate.

## Implementation plan

### Architecture: single write owner — new `fw/src/power/charger_policy.{h,cpp}`

The *only* code that writes BQ config registers after boot (EN_CHG, ICHG, IINDPM, VINDPM, EN_ICO, WATCHDOG). Driven by the existing 500ms `charger_status_thread` in `power.cpp`; `battery_service.cpp` becomes a GATT frontend calling policy setters. Own `k_mutex` (BLE writes arrive on BT RX thread). Core loop = **reconcile-on-mismatch**: read actuals each tick, rewrite only on divergence from targets, read-back verify (TRM mandate). This uniformly absorbs adapter re-plug (BC1.2 auto-INDET rewrites IINDPM), watchdog resets, and TPS-bundle writes — zero I2C writes in steady state; repeated divergence logs a warning (evidence the bundle is fighting us → rebuild bundle instead).

**Watchdog policy: disable (WATCHDOG=000) at boot, don't feed.** The watchdog's reversion target (EN_CHG=1 @ ICHG=2A) is precisely the unsafe state behind symptom 1 — POR defaults are not "safe" here. Feeding from the 500ms thread is fragile (patch download holds `task_mutex` for seconds). Autonomous protections (BATOVP, TSHUT, OCP) are unaffected. The tick still treats `WATCHDOG≠000` or `WD_STAT` as divergence and re-applies everything.

Boot sequence (`charger_policy_boot_init()`, order load-bearing): 1) disable watchdog (REG10) — first, so nothing later reverts; 2) read status → `vbat_present`; 3) program VINDPM (`CONFIG_APP_VINDPM_MV`, default 4600); 4) program ICHG from persisted value; 5) effective `EN_CHG = user_toggle && vbat_present` (**the no-battery gate**); 6) `bq25792_ibat_sense_enable(true)`.

Tick: battery-presence edges (absent→force EN_CHG off; present→re-apply user value); input budget from `tps25750_get_pd_power_info()` → IINDPM = min(contract mA, `CONFIG_APP_INPUT_CURRENT_LIMIT_MAX_MA`); for Type-C-default/unknown sources leave BC1.2's value (optionally EN_ICO via `CONFIG_APP_CHARGER_USE_ICO`); reconcile ICHG/IINDPM/VINDPM/WATCHDOG.

**Variable-voltage sources are supported upfront (user requirement).** The BQ25792 is buck-boost (3.6–24V input for the 8.4V pack), so higher-voltage PD contracts are the natural fix for the 2.5W budget — 9V/1.5A is 13.5W. Three consequences, all in PR C:
- **VINDPM is derived per-contract**, not a constant: `VINDPM = max(3600, 90% × contract_mv)` (rounded to 100mV/LSB). `CONFIG_APP_VINDPM_MV` (4600) becomes the fallback for 5V-only/non-contract sources.
- **VAC_OVP (REG10[5:4]) must be verified and programmed** to a threshold above the highest contract voltage we'll accept — its POR default was not captured during datasheet research; **verify against the datasheet REG10 table (p.68) before PR C writes anything** (if it defaults to a 7V-class threshold, a 9V contract trips input OVP).
- **The TPS25750 only negotiates contracts its bundle's sink capabilities advertise.** Experiment A reads TX_SINK_CAPS (0x33) to learn what the ACT bundle requests today; if it's 5V-only, getting >5V contracts requires rebuilding the bundle (ACT questionnaire) — a hardware-config change to schedule alongside PR C.

**ICO decision (user question: enable now?): keep plumbed but off (`CONFIG_APP_CHARGER_USE_ICO=n`).** ICO discovers *unknown/legacy* adapter capability by ramping input current until VBUS sags to VINDPM. With an explicit PD contract or Type-C 1.5A/3A advertisement the budget is already known digitally, and deliberately probing past a contract violates PD — compliant sources answer with OCP foldback or a hard VBUS cut, not a graceful sag. So ICO is only ever relevant on the Type-C-default/unknown branch; even there it needs an ICO-aware reconcile mode (with EN_ICO on, the effective limit is ICO's REG19 result, not the IINDPM register) and must be checked against the TPS25750 sink-path switch current rating (verify in TPS datasheet specs — do not assume). PR A lands the plumbing (`bq25792_ico_enable`, REG19/ICO_STAT readbacks); the on/off decision follows Experiment A's data on how often real usage sits on legacy sources.

### Firmware changes by file

**`fw/include/zephyr/drivers/InternalDeviceRegister.hpp`** — fix `flush()` to byte-swap 16/32-bit symmetric with `read()`.

**`fw/drivers/bq25792/`** (+ public header): add `xact_mutex` to dev data (write + read-back = multi-step transaction). New setters (clamped, read-back verified, −EIO on mismatch): `bq25792_set_charge_current_ma` (REG03, 10mA/LSB), `bq25792_set_input_current_limit_ma` (REG06), `bq25792_set_input_voltage_limit_mv` (REG05, needs a new UnitConversion), `bq25792_watchdog_disable`/`_feed` (REG10), `bq25792_ico_enable` (REG0F). New getters: readbacks of the above, `bq25792_get_ico_current_limit_ma` (REG19), `bq25792_get_vsys_mv` (0x3D), and `bq25792_get_status(struct bq25792_status*)` — REG1B..1E in **one** bridged burst read (4 status regs for one 4CC task), decoded to flags (iindpm/vindpm active, wd_expired, chg_stat, vbus_stat, ico_stat, vbat_present, vsysmin_regulation…), **propagating I2C errors** (unlike legacy getters).

**`fw/drivers/tps25750/`** (+ public header): read the already-defined-but-never-read host registers (plain `i2c_burst_read_dt`, not bridged, cheap): `tps25750_read_power_status` (0x3F), `_read_pd_status` (0x40), `_read_active_contract_pdo/rdo` (0x34/0x35), and `tps25750_get_pd_power_info()` decoding to `{source: none/typec-default/1.5A/3A/pd-contract, available_mv, available_ma, raw_pdo, raw_rdo}` — fixed PDOs only (bits19:10 ×50mV, bits9:0 ×10mA); PPS → conservative 5V/500mA fallback. Don't decode ChargerDetectStatus (unsupported on FW TPS25750_F509.04.02 — cite in comment; shell prints raw bytes). IRQ-driven `NewContractAsCons` dispatch deferred; 500ms polling is fine.

**`fw/src/power.cpp`**: thread startup calls policy boot-init (drops direct EN_CHG apply); loop: one `bq25792_get_status()` + ADC reads → `charger_policy_tick()` → `battery_service_update()` (+SOC) → LED. New **read-only** shell: `power bq limits` (ICHG/IINDPM/VINDPM/ICO/watchdog readbacks + all status flags — the 20mA discriminator), `power pd contract` (decoded PD info + raw hex). Later: `power bq ichg <ma>`, `power policy` (snapshot), reroute `power bq charge enable` through the policy (raw bypass would be reverted by reconcile within 500ms — note in help text).

**LED color by SOC (task 5)**: in `power.cpp`, charging branches (Breathing/FastBreathing) pick `StatusColor` by percent: <25% Red, <50% Orange, <75% Yellow, ≥75% Green; NotCharging branch refactored onto the same helper (replaces hardcoded 7700/7350mV thresholds). No status-LED API change.

**`fw/src/battery_soc.h`** (new, header-only, BT-free): `constexpr uint8_t battery_soc_percent(int32_t vbat_mv)` — piecewise-linear 2S rest-voltage table (7000mV=0% … 8400mV=100%); shared by LED logic and the percent characteristic; v1 ignores load offset (documented; follow-up: IBAT compensation).

**`fw/src/bluetooth/battery_service.cpp`** — append-only (UUIDs positional):
| pos | Name | Type | Props |
|---|---|---|---|
| 0–4 | existing telemetry | | unchanged |
| 5 | Charging Enabled | bool | RW+N persisted — now routed via `charger_policy_set_user_charge_enable()`; **accepted + persisted even with no battery** (intent persists; gating is policy's job) |
| 6 | **Charge Current (mA)** | uint32 | RW+N, persisted `"battery/charge_current_ma"`, default `CONFIG_APP_CHARGE_CURRENT_MA`; out-of-range \[50, `CONFIG_APP_CHARGE_CURRENT_MAX_MA`\] → ATT reject; modeled on `ChargeEnableCharacteristic`'s onWriteChecked pattern |
| 7 | **Battery Percent** | uint8 | R+N, from `battery_soc_percent()` |

**New `fw/src/bluetooth/power_debug_service.{h,cpp}`** — service id 6, gated `CONFIG_APP_POWER_DEBUG_SERVICE`, all read+notify, fed from the policy snapshot the tick already gathered (zero extra I2C): Input Limit (mA), Power Flags (uint8 bitmask: vbat_present, vbus_present, iindpm_active, vindpm_active, vsysmin_reg, wd_expired, charge_gated), PD Source Type (uint8), PD Available (mV), PD Available (mA), ICO Result (mA).

**Kconfig (`fw/Kconfig`)**: `APP_CHARGER_POLICY` (bool, depends on BQ25792 && TPS25750 — *not* BT; gating must work without BLE), `APP_CHARGE_CURRENT_MA` (**default 900**), `APP_CHARGE_CURRENT_MAX_MA` (default 2000), `APP_INPUT_CURRENT_LIMIT_MAX_MA` (default 3000), `APP_VINDPM_MV` (default 4600), `APP_CHARGER_USE_ICO` (default n), `APP_POWER_DEBUG_SERVICE` (depends on APP_BATTERY_MONITOR). Proto0 board conf enables policy + debug service; DK gets nothing (legacy).

### Tests (native_sim, no hardware)

**Emulator additions** (`fw/drivers/emul_tps25750/`): `bq_por_defaults()` seeding (ICHG=2A, IINDPM=3A, VINDPM=3.6V, REG10=0x05, EN_CHG=1); watchdog model (`expire_watchdog()` reverts watchdog-scoped regs + sets WD_STAT; no-op when WATCHDOG=000 — the gating regression); `simulate_plug()` (auto-INDET IINDPM rewrite model); `set_vbat_present()`; host-register backdoor for 0x34/0x35/0x3F/0x40; optional stuck-register hook for the −EIO read-back path.

**Suites**: extend `emul_tps25750` tests (16-bit bridged write round-trip — catches the flush() fix; every new setter/getter; PD-info decode table). New `fw/tests/power/charger_policy/` (policy against real drivers + emulator: no-battery boot gating, insertion/removal edges, watchdog-disabled ordering, re-plug reconcile, PD-tier→IINDPM). Extend `fw/tests/bluetooth/battery_service` (charge-current range reject/apply/persist, percent characteristic). Tiny suite for `battery_soc_percent` monotonicity/endpoints.

### App changes (companion app)

- **Slim tile**: trim `app/components/battery-card.tsx` to charge-status badge + percentage (prefer the new firmware Battery Percent characteristic; fall back to `voltageToPercent(vbatMv)` from `app/services/battery.ts` for old firmware). Whole tile wraps in `<Link href="/(tabs)/device-state/battery" asChild>`.
- **Detail page**: new `app/app/(tabs)/device-state/battery.tsx` (header/back/ScrollView shape copied from `[serviceUuid].tsx`): curated rows — voltage, current, VBUS, system/battery watts (existing `services/battery.ts` helpers), charge-status label, Charging Enabled toggle, **Charge Current (mA)** numeric input (uint32 auto-renders via existing `CharacteristicUint32`) — plus a "Power Debug" section rendering the Power Debug service's characteristics generically via `useCharacteristicEditor().renderCharacteristicInput()`.
- **Wiring**: `<Stack.Screen name="battery"/>` in `app/app/(tabs)/device-state/_layout.tsx`; `MenuRow` on `device-state/index.tsx`; exclude the Power Debug service UUID from the generic `settingsServices` bucket (it's folded into the battery page); new UUID constants in `app/constants/bluetooth.ts` (battery suffixes `0006`/`0007`, power-debug service UUID `...0006-...`).
- No new CPF formats needed (uint32/uint8/bool all supported); units stay in CUD names per convention.
- **Tests**: update `battery-card.test.tsx`, `device-state-index.test.tsx`; new battery-page test mirroring `device-state-detail.test.tsx`.

## PR breakdown (dependency order)

0. **Plan doc** — commit this file to `docs/plans/power-management-overhaul.md` (can ride with PR A).
1. **PR A — driver plumbing + read-only diagnostics** (no device behavior change): flush() endianness fix, BQ setters/getters + status burst-read, TPS PD-info getters, emulator backdoors + tests, `power bq limits` / `power pd contract` shell. → **Run Experiment A** (and optionally B); record findings in the plan doc.
2. **PR B — charger_policy core**: watchdog disable, no-battery gating (boot + runtime), battery service rerouted through policy, policy test suite. **Fixes symptom 1.**
3. **PR C — input power management**: PD contract/tier → IINDPM, VINDPM programming, reconcile/re-plug handling, optional ICO. **Fixes symptoms 2+3's budget half** (shaped by Experiment A results).
4. **PR D — configurable charge current**: Kconfig (default 900mA), Charge Current characteristic (pos 6), shell setter. **Fixes symptom 3's cap half.**
5. **PR E — telemetry & UX**: `battery_soc.h`, Battery Percent (pos 7), Power Debug service, LED color-by-SOC (**task 5**), and the app redesign (**task 4**). Splittable (E1 fw / E2 app) if review size demands.

Each firmware PR goes through `/submit-pr` (both boards, tests, ≥50% patch coverage); PRs touching the device↔app surface need on-device + app verification. Hardware sessions take the `board` (and `app` when driving the phone) locks per the hw-lock rules.

## Verification

- **Pre-hardware**: `/build-proto0` + `/build-dk` + `/test-fw` per PR; `/validate-app` (jest + tsc + eslint) for app changes.
- **Hardware**: Experiment A dumps before (PR A) and after (PR C/D) — expect: faceplate attached + PD charger → fast-charge at configured ICHG with IINDPM_STAT clear; no battery + charge toggle ON → boots stably, `charge_gated` flag set. `/flash-and-verify` for each on-device pass; app verified against a live board for PR E.

## Risks / open questions

- **TPS bundle may fight host writes** (shared I2Cm queue, TRM p.60). Reconcile handles occasional writes; a persistent fighter means rebuilding the ACT bundle (e.g. 900mA charge current baked in, or switching to `tps25750-no-bq.c` and owning everything host-side). Experiment A characterizes this before PR C/D lock in an approach.
- **Charging-Enabled accepted-but-gated with no battery**: chosen so the app toggle stays usable on the bench; `charge_gated` debug flag exposes it. (Alternative — ATT reject — was rejected as a UX trap.)
- **VAC_OVP POR default unverified** — must be read from the datasheet REG10 table before PR C programs >5V support (see PR C section).
- **ACT bundle sink caps may be 5V-only** — Experiment A's TX_SINK_CAPS read decides whether a bundle rebuild is needed to unlock higher-voltage contracts.
- **SOC from rest-voltage is inaccurate under load** (1.2–1.8W faceplate); v1 accepts it, follow-up is IBAT compensation.
- **`power bq charge enable` semantics change** (routed through policy; raw writes get reconciled away within 500ms) — flag in help text/release notes.
- Charger-thread stack (1024) and 2Hz bridged-I2C budget re-measured on hardware after PR C (mitigation: rotate readbacks one-per-tick).
