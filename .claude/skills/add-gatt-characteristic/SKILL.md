---
name: add-gatt-characteristic
description: "Add or modify a firmware BLE GATT service or characteristic (animation parameters or app services like battery/config), including the companion-app side (constants, codec, UI component). Use for: new characteristic, new GATT service, expose value over Bluetooth, new setting/toggle in the app, new CPF format. GATT layout is a compatibility surface — read this before touching any bt_service_cpp.h-based service."
---

# Add a BLE GATT service / characteristic

Firmware half below; **the companion-app half (constants, codec, component, tests) is in
[references/app-side.md](references/app-side.md)** — a device↔app change is not done until both
halves land. Read `fw/CLAUDE.md` ("Bluetooth / GATT layer") first if you haven't.

## COMPATIBILITY RULES — read before editing any service

The GATT table of shipped firmware is a compatibility surface. Violations break real phones:

1. **NEVER reorder providers in a `BtGattServer(...)` argument list.** Auto-UUID assignment is
   positional (`composeAutoCharacteristicUuid` in `fw/src/bluetooth/bt_service_cpp.h` stamps the
   declaration-order index into the UUID), so reordering silently changes characteristic UUIDs
   AND the metadata-blob entry order the app zips positionally. This compiles clean and can even
   render fine on a fresh, unbonded connection (a pure reorder stays self-consistent — labels
   follow handles), but the app's fixed UUID constants (`UUID_BATTERY_*` etc. in
   `app/constants/bluetooth.ts`) now silently read the WRONG characteristics, and nothing at
   runtime detects it — the app's entry-count check catches only a count mismatch, never a
   same-count reorder (see the "ORDERING ASSUMPTION" comment in `app/hooks/use-ble-connection.ts`).
2. **Append, never insert/remove/reorder, in shipped firmware.** Android caches GATT handles per
   bonded device; any table restructure breaks bonded phones (issue #115; symptom:
   connected/encrypted link with ATT MTU stuck at 23). Impact is phone-stack-dependent: compliant
   stacks auto-recover via Service Changed + DB hash, and the app already passes
   `refreshGatt: "OnConnected"` — but OxygenOS-class stacks honor neither and only forget+re-pair
   recovers, so assume the worst for shipped firmware. New characteristics go at the END of the
   provider list; new services are new tables and are safe. Details + diagnosis → /debug-ble.
3. **Settings keys are explicit stable string literals** (e.g. `"battery/charge_enable"`), never
   derived from declaration position — UUIDs may be positional but persisted keys must survive
   any future (pre-ship) reorder.

## Which kind of characteristic?

- **Parameter on an animation** → the characteristic lives in that animation's adapter,
  `fw/src/bluetooth/animation_adapters/<name>_animation_bt.cpp`, as a
  `BtGattPersistentCharacteristic<"<name>/<param>", "Label", Notify, T, Default>` appended to its
  `BtGattServer(...)` list. Full adapter anatomy (deps struct, registrar, registry) → /add-animation.
- **New app-level service** (battery/config-like) → this skill, next section.
- **Doesn't fit the template model** (write-without-response command streams, raw offsets) → raw
  `BT_GATT_SERVICE_DEFINE`; precedent with rationale comment: `fw/src/bluetooth/mcuboot_updater_service.cpp`.

## New app service — copy `fw/src/bluetooth/battery_service.cpp`

That file (137 lines) is the canonical shape: primary service + auto characteristics +
one write-hooked characteristic + `BT_GATT_SERVER_REGISTER`. Steps:

1. **Pick the service id.** App services use
   `BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, <id>, 0x56789abc0000))`.
   Derive the next free id — do NOT trust a remembered number, an in-flight branch may have
   claimed one:
   ```bash
   grep -rn "0x56789abc0000" fw/src/        # literal ids (3,4,5 as of 2026-07 — re-verify)
   grep -rn "kServiceIdNum" fw/src/         # ids behind constants (1=CoreConfig, 2=AudioConfig)
   ```
   (Animation services use a different macro/suffix: `BT_ANIMATION_SERVICE_UUID(anim_id)` →
   `...anim_id<<8...56789abd0000`. Extensions occupy group `0x40+slot` — see
   `fw/src/extensions/extension_limits.h` static_asserts.)
2. **Declare providers** in a `.cpp` under `fw/src/bluetooth/`:
   `BtGattPrimaryService<kUuid>` first, then characteristics. Aliases (bottom of
   `bt_service_cpp.h`): `BtGattAutoReadNotifyCharacteristic<"Label", T, Default>`,
   `BtGattAutoReadWriteCharacteristic`, etc.; persisted values use
   `BtGattPersistentCharacteristic` (`fw/src/bluetooth/persistent_characteristic.h`). All
   persisted characteristics share one fixed-cap registry — `kMaxRegistryEntries = 96` in
   `fw/src/settings/persistent_value_registry.cpp` (issue #114 tracks removing the cap); a full
   table drops new entries at boot (`LOG_ERR` "Persisted value table is full", `-ENOMEM` — no
   build error), so check remaining headroom when adding persisted characteristics.
   The CUD label + CPF format (deduced from `T`) are what the app auto-renders as a control —
   no app change needed for standard types. Assemble with `BtGattServer server(primary, a, b, ...);`
   then `BT_GATT_SERVER_REGISTER(nameStatic, server);` — registration is link-time, no init call.
3. **Gate it**: `CONFIG_APP_<NAME>` bool in `fw/Kconfig` (copy `APP_BATTERY_MONITOR`'s entry,
   `depends on BT` + hardware deps), one
   `target_sources_ifdef(CONFIG_APP_<NAME> app PRIVATE src/bluetooth/<file>.cpp)` in
   `fw/CMakeLists.txt` (grep `APP_BATTERY_MONITOR` there for the spot), enable with `=y` in
   `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf` — the flat per-board conf
   (the legacy DK board on the `dk-support` branch shed such features there for budget).
4. **Metadata blob is automatic.** `BtGattServer` appends a per-service metadata characteristic
   (fixed UUID `...cccc-56789abd0000`) when `CONFIG_APP_BT_METADATA_CHARACTERISTIC=y`.
   `kMetadataBlobVersion` (`bt_service_cpp.h`) and `METADATA_BLOB_VERSION`
   (`app/constants/bluetooth.ts`) are in lockstep — if you ever change the blob layout, bump BOTH.

UUID gotcha: the first auto-assigned characteristic UUID has suffix `...0000` — **identical to
the service UUID itself** (see `UUID_BATTERY_SERVICE` == `UUID_BATTERY_VOLTAGE` in
`app/constants/bluetooth.ts`). Valid GATT; don't "fix" it.

## Write-path rules (the ones that bit real PRs)

- **Fallible writes** (side effect can fail, e.g. an I2C register write): you cannot subclass
  `BtGattPersistentCharacteristic` — its CRTP `Self` is itself, subclass hooks never dispatch.
  Copy `ChargeEnableCharacteristic` in `battery_service.cpp` instead: subclass
  `BtGattAutoCharacteristicExt<YourClass, "Label", Notify, ReadOnly, T, Default>` directly,
  hand-register with `persistent_value_registry_register` if persisted, and implement
  `int onWriteChecked(const T&)`. Non-zero return ⇒ framework restores the previous value and
  fails the ATT op with `BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED)`. Define `onWrite` OR
  `onWriteChecked`, never both (static_assert). `onWriteChecked` is unsupported for string-backed
  types (`BtGattString<N>` — long writes land chunk-wise; static_assert in `_write`).
- **NEVER ACK a write whose hardware side effect failed and "fix up" with a corrective notify.**
  Banned (hardware-verified on PR #89, re-affirmed in #106; see "Refusing a GATT write" in
  `fw/CLAUDE.md`): the notify reaches the phone before the write response, so the app's
  optimistic update lands last and clobbers the correction. Reject with the ATT error above —
  the app's catch-and-revert then restores the UI deterministically.
- **ReadOnly characteristics must not advertise WRITE.** Handled by the `ReadOnly` template
  parameter (properties byte in `BtGattCharacteristicCommon`); use the `ReadOnly`/`ReadNotify`
  aliases rather than hand-building a `bt_gatt_chrc` (regression = app renders editable inputs
  for unwritable values, PR #106).
- **`notify()` for string types sends the `strnlen` length, not `sizeof(storage_)`** — already
  handled via `BtGattNotifyTraits`; don't regress it when adding characteristic types (a
  full-capacity notify exceeds ATT MTU and fails with -12).
- Non-mixin persistence precedent (persist by NAME, not index): `BtGattDropdownList` in
  `fw/src/bluetooth/animation_adapters/glim_player_animation_bt.cpp`.

## Validate

1. Firmware: `/build-proto0` must compile clean.
2. Add a native_sim test via /add-fw-test — for write hooks copy
   `fw/tests/bluetooth/checked_write/` (drives the characteristic's static `write()` callback
   directly, no BT stack); for a whole service copy `fw/tests/bluetooth/battery_service/`.
3. App half: follow [references/app-side.md](references/app-side.md), then /validate-app.
4. PR via /submit-pr — it **requires on-device + companion-app verification for any change
   touching device↔app communication** (this skill's changes always do). That step needs the
   board: /flash-and-verify. If hardware isn't available, say so plainly in the PR body
   ("device/app verification: not exercised — <reason>"); never claim verification you didn't do.
