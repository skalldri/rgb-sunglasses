# Companion-app side of a GATT change

The app auto-renders any firmware characteristic whose CPF format it already knows (CUD label +
CPF format come from the firmware's metadata blob or descriptors) — **a new characteristic of an
existing type usually needs zero app code**, only the UUID constant if code addresses it directly.
This file covers the cases that DO need app work: a new CPF format/component, a new service, or a
new protocol client. Validation for all of it: /validate-app (don't hand-roll jest/tsc commands).

## Files (all under `app/`)

| Concern | File |
|---|---|
| UUID + format constants | `constants/bluetooth.ts` |
| base64 wire codecs | `services/ble-value-codec.ts` |
| Per-format input components | `components/characteristic-<type>.tsx` |
| Render dispatch | `renderCharacteristicInput()` in `hooks/use-characteristic-editor.tsx` |
| Global BLE store / write API | `context/bluetooth-context.tsx` |
| Discovery, metadata fast path | `hooks/use-ble-connection.ts` |
| Tests | `__tests__/` |

If any doc still cites a single-file "device-state.tsx" (with or without line numbers), it is
stale — `app/(tabs)/device-state/` is a directory (`index.tsx` + `[serviceUuid].tsx`) and the
render dispatch lives in `use-characteristic-editor.tsx`. (`app/CLAUDE.md` is already correct
on this as of 2026-07.)

## Adding a new CPF characteristic type

1. **Constant** in `constants/bluetooth.ts`. Custom formats use the `0xE0+` range and MUST match
   `fw/src/bluetooth/gatt_cpf.h` — precedents: `0xE0` `BLE_GATT_CPF_FORMAT_CUSTOM_COLOR`
   (fw name: `..._RGB888`), `0xE1` `BLE_GATT_CPF_FORMAT_DROPDOWN_LIST`. Cite the fw header in a
   comment next to the constant, as the existing ones do.
2. **Codec** in `services/ble-value-codec.ts`: pure exported functions over base64 strings,
   multi-byte values little-endian, **throw `Error` on short payloads** (copy
   `encodeUint32ToBase64`/`decodeFloat32FromBase64`; exception: `parseMetadataBlob` returns
   `null` — callers treat that as "fall back").
3. **Component** `components/characteristic-<type>.tsx`. Two contracts exist — pick deliberately:
   - **Parent-managed** (copy `characteristic-uint32.tsx`): props
     `{charUuid, charInfo, pendingValue, onChangeText(charUuid, text), onWrite(charUuid, encoded, previous)}`;
     the editor hook owns pending state and the write.
   - **Self-writing** (copy `characteristic-dropdown.tsx`): props `{charUuid, charInfo}`, calls
     `useBluetooth().writeToCharacteristic` itself. Pass `{ skipOptimisticUpdate: true }` when
     the device canonicalizes the written value and notifies it back (the dropdown does — the
     firmware reorders the option list); the default optimistic path would show a wrong value
     until the notification lands.
4. **Dispatch**: add a `charInfo.cpfFormat === ...` branch in `renderCharacteristicInput()`
   (`hooks/use-characteristic-editor.tsx`). Note the guard above the dispatch: characteristics
   with both `isWritableWithResponse === false` and `isWritableWithoutResponse === false` render
   `CharacteristicReadonly` regardless of format. If the type is text/numeric user-editable, also
   extend the two `useEffect`s (initial pendingValues + notification sync) and
   `decodeValueForInput()` in the same file.
5. **Tests** in `__tests__/`: codec cases in `ble-value-codec.test.ts`, a component test copying
   `characteristic-dropdown.test.tsx`. Keep component display names stable
   (`CharacteristicBoolean` etc.).

## Adding a new service's constants

Copy the Battery block in `constants/bluetooth.ts`: `UUID_<NAME>_SERVICE` plus one constant per
characteristic, suffixes `...0000, 0001, ...` **matching firmware declaration order** (positional
— see the COMPATIBILITY RULES in SKILL.md; the first characteristic's UUID equals the service
UUID, that's expected). Non-animation services also need a display-name entry in
`KnownServiceIds` (animation services are named live via their Animation Name characteristic —
never add them to that map).

## Traps (each one is a shipped bug, don't re-ship it)

- **UUID collision trap**: `UUID_IS_ACTIVE_CHARACTERISTIC` and `UUID_ANIMATION_NAME_CHARACTERISTIC`
  are intentionally the same literal UUID in EVERY animation service. Never look them up or write
  them via the flat `characteristics` map / bare `writeToCharacteristic` — use
  `characteristicsByService` + `writeServiceCharacteristic(serviceUuid, ...)`.
  `writeCharValue()` in `use-characteristic-editor.tsx` already routes this correctly.
- **Optimistic updates land synchronously BEFORE the `await`**, and reverts are compare-and-swap
  inside a functional setState updater (PR #98) — both write helpers in `bluetooth-context.tsx`
  already do this; a naive "set flag, await, then update" reintroduces UI flicker and can clobber
  a mid-write device notification with a stale revert.
- **Read current value, THEN subscribe, on every (re)connect** (PR #78): subscribing alone leaves
  stale UI after reconnect because an unchanged value fires no notification. Pattern:
  `use-ble-connection.ts` (per-characteristic `characteristic.read()` during discovery) and
  `services/mcuboot-updater-client.ts`.
- **A firmware write rejection surfaces on Android as `androidErrorCode: 252` (0xFC) with
  `attErrorCode: null`** (PR #100) — probe both fields plus the reason string;
  `services/ble-errors.ts` (`describeWriteError`) already does, route new error handling through it.
- **New protocol clients** (own characteristic + notification stream): copy the class pattern of
  `services/mcumgr.ts` / `services/mcuboot-updater-client.ts` — constructor takes `Device`,
  `initialize()` discovers + subscribes, and ALL requests serialize through a promise
  `requestChain` (grep `requestChain` in `services/mcumgr.ts`). Single
  `responseResolver`/`responseRejecter` fields break on overlapping requests (PR #55: spurious
  "SMP request timeout"); `destroy()` must fail-fast queued-but-unstarted requests.

## Done?

/validate-app must pass, then back to SKILL.md's Validate section — /submit-pr requires on-device
+ app verification for device↔app changes (/flash-and-verify).
