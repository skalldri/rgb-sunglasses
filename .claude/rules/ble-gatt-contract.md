---
paths:
  - "fw/src/bluetooth/bt_service_cpp.h"
  - "fw/src/bluetooth/bt_gatt_traits.h"
  - "fw/src/bluetooth/gatt_cpf.h"
  - "fw/src/extensions/extension_bt.{h,cpp}"
  - "fw/src/extensions/extension_metadata_blob.*"
  - "fw/src/animations/color_mode_source.h"
  - "app/constants/bluetooth.ts"
  - "app/hooks/use-ble-connection.ts"
  - "app/context/bluetooth-context.tsx"
  - "app/services/ble-value-codec.ts"
  - "app/services/ble-errors.ts"
  - "app/components/characteristic-color.tsx"
  - "app/app/color-picker-modal.tsx"
---

# BLE GATT contract (firmware ↔ app)

Loads when you read either side of the wire contract. The GATT layout is a compatibility
surface: changing it is `/add-gatt-characteristic` territory. Firmware-only GATT internals:
`.claude/rules/fw-bluetooth-gatt.md`; app connect/discovery: `.claude/rules/app-ble-connection.md`.

## Refusing a GATT write: return an ATT error, never "success + corrective notify"

**Hardware-verified on PR #89.** Firmware must reject an unacceptable write with
`BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED)` (or use the `onWriteChecked` hook, which does it
for you), never accept it and then notify a different value. Precedent: `write_is_active` in
`fw/src/extensions/extension_bt.cpp`.

Why: the app applies an optimistic update and reverts it in its write `catch`. Historically
the optimistic update ran in the write's `.then()`, and a notification sent from inside the
write handler reached the phone *before* the write response — so the optimistic update landed
last and clobbered the corrective value; the UI showed the write as accepted. Since the
issue-#91 fix moved the optimistic update to *before* the `await` (see `app/CLAUDE.md` "State
Updates with Optimistic UI"), a corrective notify would no longer be silently lost — but
rejecting with an ATT error remains the required contract: it is what drives the app's
deterministic revert and avoids a visible flash-then-correct. Notifications are the right tool
only for state changes that originate device-side after the write completed (e.g. an extension
sandbox fault flipping Is Active off).

## Bulk metadata characteristic (issue #41 follow-up)

Firmware (`fw/src/bluetooth/bt_service_cpp.h`, gated by `CONFIG_APP_BT_METADATA_CHARACTERISTIC`):
`BtGattServer` automatically synthesizes and appends one extra read-only characteristic per
service (fixed shared UUID `kMetadataCharacteristicUuid`, same pattern as
`kAnimationNameCharacteristicUuid`) whose value is a compile-time-constant packed blob
containing every sibling characteristic's CUD name + CPF format:
`[version][entry_count]` then `[cpf_format][name_len][name_bytes]` per entry, in `Providers...`
declaration order. **No per-service `.cpp` needs to change** — the blob derives from the same
`Providers...` pack each service already passes into `BtGattServer(...)`, via
`getDescription()`/`getCpf()` static accessors on `BtGattCharacteristicCommon` and the
`BtGattMetadataBearingProvider` concept + `MetadataBlobBuilder<Ps...>` fold (just above
`BtGattServer`), which skip the primary-service provider automatically. When the symbol is
disabled, `getMetadataAttrsTuple()`/`kMetadataBlob` are never referenced and so never
instantiated — zero flash cost (the legacy DK board, now on the `dk-support` branch, disabled
it because the duplicated CUD strings overflowed its internal-flash slot).

Extension services (`fw/src/extensions/extension_metadata_blob.{h,cpp}`, issue #90 follow-up)
build a runtime mirror — same wire format, same fixed UUID/version — threaded through
`extension_bt.cpp`'s `append_characteristic()` so blob order can never drift from GATT handle
order. Cost there is a few hundred bytes of RAM per extension slot, not flash.

App (`app/hooks/use-ble-connection.ts`): per service, `connect()` first looks for a
characteristic matching `UUID_METADATA_CHARACTERISTIC` (`app/constants/bluetooth.ts`), reads it
once, parses it with `parseMetadataBlob()` (`app/services/ble-value-codec.ts`), then zips it
_positionally_ onto that service's characteristic list — replacing 2 descriptor reads (CUD +
CPF) per characteristic with 1 read per service. It falls back automatically to the
per-descriptor path on any read failure, blob-version mismatch, or entry-count mismatch
(logged, never silently mis-zipped), which is also what happens for services without the
characteristic (e.g. the third-party McuMgr service). Extension services needed no app change.
Hardware-verified: discovery across the (then) 9 services dropped from ~13-30 s to ~6 s with
zero fallback/mismatch warnings.

**Ordering assumption.** The positional zip assumes `characteristicsForService()` returns
characteristics in firmware GATT declaration order. This holds because ATT "Read By Type"
(used by characteristic discovery) is spec-required to return attributes in ascending handle
order, and handles are assigned in exactly `Providers...` declaration order. Verified that
react-native-ble-plx's Android module (`BleModule.java` → `Service.java`) passes Android's
native `BluetoothGattService.getCharacteristics()` result through with no re-sort. A same-count
_reordering_ would not be caught (only a count mismatch is) — that residual risk is accepted
rather than paying for a per-entry UUID-tagged wire format. The full chain is also in the
ordering comment blocks of `use-ble-connection.ts` and `bt_service_cpp.h`.

## Notify payloads must fit the connection's *current* MTU

A single `bt_gatt_notify()` cannot be fragmented the way long reads/writes can; the whole
value must fit one ATT PDU bounded by the connection's *current* MTU. A notify that doesn't
fit fails firmware-side only (`bt_att: No ATT channel for MTU ...` / `Notify failed: -12`); the
app just never sees the new value. Symptom playbook: `/debug-ble` (`references/notify-mtu.md`).

- **Firmware sends the actual string length** for string-backed types (`BtGattString<N>` /
  `BtGattDropdownList<N>`), matching `read()`'s `strnlen()` — not `sizeof(storage_)`. Before
  this fix a `BtGattDropdownList<512>` (e.g. `GlimSelectionCharacteristic`) always tried to
  notify the full 512-byte buffer, so every notify failed even for a ~28-byte list. On
  failure `notify()` logs the characteristic's `Description` and attempted length.
- **`BtGattNotifyTraits<BtGattDropdownList<N>>` also caps the first-token length at 20 bytes**
  (`kGuaranteedSafeNotifyLen` in `fw/src/bluetooth/bt_gatt_traits.h`). A GLIM filename token
  can be up to `kMaxNameLen` (32, `fw/src/storage/glim_registry.h`) bytes, and
  `payload + 3-byte ATT header` can exceed the MTU whenever it hasn't (yet, or ever) been
  negotiated above the 23-octet floor (`BT_ATT_DEFAULT_LE_MTU` in Zephyr's `att_internal.h`; an MTU Exchange requesting less
  is rejected, so 23 is a hard floor). That window isn't rare: any momentarily un-negotiated
  connection, and durably the stale-GATT-cache split-brain (`bt_state` shows `ATT MTU: 23` on a
  healthy-looking `CONNECTED`/`L4` link). 20 bytes (23 − 3) always fits. This costs nothing
  because the app treats any dropdown-list notification as "go re-read" (the `DROPDOWN_LIST`
  branch in `use-ble-connection.ts`).
- **The app requests a larger MTU**: `connect()` calls `requestMTU(247)` as its own awaited,
  non-fatal step after the link is up. Without it the link stays at the BLE default `ATT_MTU` of 23 (~20 usable bytes).
  Even at 247, a notifiable characteristic whose *content* can grow past ~244 bytes (e.g.
  `Glim Selection` with many GLIM files) needs a bigger MTU, a smaller payload, or
  read-after-notify. iOS negotiates 293 on its own (`requestMTU` is a no-op there).

## Connection interval (issue #41, issue #188)

The app's discovery walk is ~170 sequential GATT operations (one
`descriptorsForCharacteristic`/`descriptor.read()`/`characteristic.read()` round-trip each on the
per-descriptor path; Android allows one outstanding op per connection), so each costs roughly one
connection interval. Neither side gets a fast
interval by default (Zephyr's unrequested default is `BT_GAP_INIT_CONN_INT_MIN/MAX`, 30-50 ms).

- **App:** `connect()` calls `requestConnectionPriority(ConnectionPriority.High)` right after
  `connectToDevice()` resolves (Android-only, try/caught, non-fatal) — roughly a 3-4x cut.
- **Firmware:** with `CONFIG_APP_BT_CONN_PARAM_GOVERNOR` (default y) the governor in
  `fw/src/bluetooth.cpp` owns parameter requests: FAST (`fast_conn_param`, 7.5-15 ms) at
  connect and during MCUmgr uploads (`fast_conn_param = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400)`), MEDIUM (30-45 ms) while the user interacts, SLOW
  (150-165 ms + latency 2) after a quiet window. With the governor off, a one-shot
  `bt_conn_le_param_update(ctx->conn, &fast_conn_param)` right before CONNECTING → CONNECTED
  is the fallback. Either side's request alone should produce the fast interval — having both
  is belt-and-suspenders. A non-zero return is logged, non-fatal. SLOW is only stable with the
  netcore's 500 ppm sleep-clock fragment (see the comment above `slow_conn_param`).
- **Measured, not assumed** (via `bt_conn_info` and the `le_param_updated` callback — see
  `.claude/rules/fw-bluetooth-gatt.md`): the interval briefly converges to 7.5 ms right after
  CONNECTED, has a ~400 ms excursion to 45 ms about 2 s in (during `connectToDevice()` / GATT
  refresh, before the read loop), then settles at **15 ms** — the slow end of the requested
  range. That is very likely Android's own `CONNECTION_PRIORITY_HIGH` floor (~11.25-15 ms),
  which as GAP central it has final say over; there is no faster public Android tier. iOS has
  no priority API and discovery takes ~30-55 s there even with the bulk-metadata path.

## UUID scheme

- App-level services: `12345678-1234-5678-000N-56789abc0000` (`app/constants/bluetooth.ts`).
- Animation services: `BT_ANIMATION_SERVICE_UUID(anim_id)` =
  `12345678-1234-5678-{anim_id<<8:04x}-56789abd0000` (`fw/src/bluetooth/bt_service_cpp.h`), e.g.
  `Animation::Rainbow = 5` → group `0500`. Animation enum values: `fw/src/animations/animation_types.h`.
- **Is Active is the fixed shared UUID `12345678-1234-5678-bbbb-56789abd0000` in every animation
  service**, built-in and extension alike (`kIsActiveCharacteristicUuid`), so the same literal
  appears once per service. **Always disambiguate by the characteristic's `serviceUUID`, never
  by `charUuid` alone.** The metadata characteristic is the fixed `…-cccc-…`.
- Parameter characteristics get auto UUIDs `…-{group}-56789abd0001/0002/…` in declaration
  order. Extension animations (ids `0x40 + slot`) → groups `4000`, `4100`, …, params in manifest
  declaration order (ids start at 1).

## Color encoding

RGB colors are uint32, wire bytes little-endian `b,g,r,mode`. Byte 3 is the **color mode**
(issue #259), mirroring `ColorMode` in `fw/src/animations/color_mode_source.h`:

| mode | meaning | bytes 0-2 |
|---|---|---|
| 0x00 | Static | `b,g,r` — the color |
| 0x01 | Spectrum Sweep | byte 2 (r) = speed 0-255, others 0 |
| 0x02 | Random on Beat | reserved (0) |
| 0x03 | Random on Activate | reserved (0) |
| 0x04 | Random Timer Fade | byte 2 (r) = speed 0-255, others 0 |

**Any unknown mode byte decodes as Static** — including 0xFF, the upper byte of the
`0xFFFFFFFF` default persisted on pre-#259 devices. Custom CPF format
`BLE_GATT_CPF_FORMAT_CUSTOM_COLOR` (0xE0) marks these. Mode-aware codec:
`encodeColorValueToBase64`/`decodeColorValueFromBase64` (`ColorValue = {mode, rgb, speed}`);
the legacy `encodeColorToBase64`/`decodeColorFromBase64` remain as byte-identical static-mode
wrappers — all in `app/services/ble-value-codec.ts`. HSV↔RGB conversion lives in
`app/app/color-picker-modal.tsx`; mode constants + labels in `app/constants/bluetooth.ts`.
All BLE values are base64 on the app side (numbers little-endian).
