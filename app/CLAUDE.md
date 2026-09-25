# app/CLAUDE.md — RGB Sunglasses companion app

React Native Expo app for controlling the RGB sunglasses over Bluetooth Low Energy: color customization, animation
control, firmware updates and extension management. Detail for each code area lives in a path-scoped rules file that
loads when you read that code — see the index at the bottom.

## Architecture

### Core Pattern: BLE GATT Characteristic-Based State Management

The app mirrors the device's GATT characteristics as UI controls. Each characteristic (boolean, uint32, float32, string,
dropdown, or custom color) automatically renders as the matching input (Switch, TextInput, picker, ColorPicker) — the
firmware adds a characteristic and the app renders it with no app change.

**Critical files:**

- `app/context/bluetooth-context.tsx` — global BLE state (`BluetoothContextDevice`), writes, clamp read-back.
- `app/app/(tabs)/device-state/` — a **directory**: `index.tsx` is the Controls menu (service list), `[serviceUuid].tsx`
  the per-service detail screen, plus `battery`, `capture`, `audio`, `audio-calibrate`.
- `app/hooks/use-characteristic-editor.tsx` — `renderCharacteristicInput()` switches on CPF format to pick a control;
  `decodeValueForInput()` decodes base64 values per format; `pendingValues` holds in-progress text edits.
- `app/hooks/use-ble-connection.ts` — `connect()`: link, MTU, discovery, monitors, reconnect loop.
- `app/constants/bluetooth.ts` — CPF format constants and UUID mappings; `app/services/ble-value-codec.ts` — codecs.

### Data Flow

1. **Connection**: `connect()` (triggered from `app/components/bluetooth-device-list-item.tsx`) discovers services and
   characteristics, reading each service's bulk metadata characteristic for names + CPF formats and falling back to
   per-characteristic CUD/CPF descriptor reads (`.claude/rules/ble-gatt-contract.md`).
2. **Rendering**: `renderCharacteristicInput()` renders a `Characteristic*` component from `app/components/`.
3. **Updates**: writes use `writeWithResponse()` with an optimistic update that reverts on error (below).
4. **Encoding**: all BLE values are base64 (`btoa`/`atob`), even booleans; numbers are little-endian. Colors are uint32
   `b,g,r,mode` with byte 3 a color mode; custom CPF `0xE0` marks them (table: `.claude/rules/ble-gatt-contract.md`).

UUIDs: app services are `12345678-1234-5678-000N-56789abc0000`; animation services `…-{id<<8}-56789abd0000`, and Is
Active is one fixed UUID shared by every animation service — match by service, never by characteristic UUID alone.

### Routing (Expo Router, file-based)

- Tabs `app/app/(tabs)/`: `bluetooth.tsx` (scan/connect), `device-state/` (above), and a hidden `index.tsx`.
- Modals: `app/app/color-picker-modal.tsx` (query params `charUuid`, `r`, `g`, `b`, `mode`, `speed` — `mode`/`speed` for
  the issue #259 color modes) and `app/app/app-update-modal.tsx` (Android self-update).
- **Firmware update is a nested stack** (`app/app/firmware-update/`: `index`, `flow`, `debug`, `extensions`) under a
  `_layout.tsx` that mounts the MCUmgr client and the release lookup once — pushed screens never unmount the one below
  (`.claude/rules/app-firmware-update.md`).

### Key technologies

- **MCUmgr (SMP)** in `app/services/mcumgr.ts` for firmware updates and extension files
  (`.claude/rules/app-firmware-update.md`, `.claude/rules/extension-file-management.md`).
- **react-native-ble-plx** with a singleton `bleManager` (`app/hooks/ble-manager.ts`). **It is patched**
  (`app/patches/react-native-ble-plx+3.5.0.patch`, applied by `postinstall`) — check the patch before upgrading
  (`.claude/rules/app-ble-connection.md`).
- React Native Reanimated is required for navigation animations; its Metro bundler warnings are safe to ignore.

## Development workflow

- **Run the app on a phone only via `/launch-app`** (`app/scripts/launch-app.sh` as a background task, `app` lock
  held) — never `npx expo run:android` or `npx expo start` directly. In a worktree, `npm ci` first.
- iOS builds only on a Mac; the Simulator has no BLE (`.claude/rules/app-ios.md`, `.claude/rules/app-release-ci.md`).
- Driving the UI on the phone: `/drive-app`. Re-pairing: `/re-pair`. **Any connected phone is viable**; per-phone
  quirks: `.claude/skills/drive-app/references/phones.md`.

### Device-Free Validation Loop

For any app change, run `/validate-app`: `npm ci` in `app/` (reapplies the ble-plx patch), then jest + typecheck + lint.
There is **no `typecheck` npm script** — it's `npx tsc --noEmit`. CI (`.github/workflows/app-ci.yml`) gates all three,
and any lint warning fails it; still run them locally. To exercise UI without a board, use the jest suites and their
mocks (`app/test/firmware-mocks.tsx`). **Green here is not "verified"** — see the next section.

### What the device-free loop CANNOT catch: feedback between a write and the state it produces

Jest mocks `useBluetooth`, so context updates never produce a new context object, and any bug in the loop "effect
reads/writes a characteristic → context re-renders with fresh objects → effect re-runs" is invisible to green tests.
Two such bugs shipped into a PR and were caught only on hardware (`.claude/rules/app-ble-state-writes.md`). Rules:

1. **Any effect that both reads a characteristic and writes the result into context must take its inputs from
   `useRef`, and may depend only on values its own writes cannot change** — a plain string like `selectedDevice?.mac`
   is fine; anything context-derived (a `charInfo`, the device object, a context callback) closes the loop.
2. **In any fire-and-forget deferred BLE callback, optional-call (`read?.()`) AND wrap in `try/catch`**, not just
   `.catch()` — a torn-down characteristic throws synchronously.
3. **A deferred read must compare-and-swap before it applies** — return `null` from the functional update when the
   current value is no longer the one you wrote.

Test that a read does NOT re-fire on a fresh device object, and check steady-state traffic on a real device before
merge (`/submit-pr` step 5).

## Common Patterns

### Adding New Characteristic Types

Follow `/add-gatt-characteristic` (app steps in its `references/app-side.md`) — do not improvise. The files involved:
the CPF constant in `app/constants/bluetooth.ts`; decode + dispatch in `app/hooks/use-characteristic-editor.tsx`;
encode helpers in `app/services/ble-value-codec.ts`; the per-format component `app/components/characteristic-*.tsx`.

### State Updates with Optimistic UI

Always follow the pattern in `writeToCharacteristic` / `writeServiceCharacteristic` (`app/context/bluetooth-context.tsx`):
apply the optimistic value **before** awaiting the write, and revert **compare-and-swap** style — only if the value is
still the one you wrote — so a notification that landed mid-write isn't clobbered:

```typescript
const previousValue = charInfo.value ?? "";
updateCharFields(charUuid, { isUpdateInProgress: true, lastWriteError: null });
updateCharValue(charUuid, newEncodedValue);            // optimistic, same render as the in-progress flag
try {
  await charInfo.characteristic.writeWithResponse(newEncodedValue);
  scheduleClampReadBack(/* … */);                       // device may clamp; read back (CAS-guarded)
} catch (error) {
  updateCharFields(charUuid, cur => cur.value === newEncodedValue ? { value: previousValue } : null);
} finally {
  setCharUpdateInProgress(charUuid, false);
}
```

The device then notifies its actual value, which wins. Firmware must reject bad writes with an ATT error, never
"success + corrective notify". Why the ordering matters (issue #91) and the rest of the behaviour:
`.claude/rules/app-ble-state-writes.md`.

## Where the detail lives

Path-scoped rules load automatically when you Read a matching file; Read them by hand when planning (subagents may not
load them).

| Area | Rules file |
|---|---|
| BLE wire contract shared with firmware (metadata blob, MTU/notify, conn interval, UUIDs, colors, refused writes) | `.claude/rules/ble-gatt-contract.md` |
| Scan / connect / discovery / reconnect + FGS / split-brain triggers / ble-plx patch / permissions | `.claude/rules/app-ble-connection.md` |
| Optimistic writes, notifications, write→state loop, audio telemetry, render-isolation tests | `.claude/rules/app-ble-state-writes.md` |
| Firmware update: SMP, hash verification, no rollback, release lookup | `.claude/rules/app-firmware-update.md` |
| Extension install/list/delete (with the firmware side) | `.claude/rules/extension-file-management.md` |
| iOS BLE differences, text-input commit, state restoration | `.claude/rules/app-ios.md` |
| Config plugins ("ensure, not add"), `.dev` variant and `--app-id` | `.claude/rules/app-config-plugins.md` |
| CI, TestFlight signing, Play variant, self-update gating | `.claude/rules/app-release-ci.md` |

| Hardware task | Skill / reference |
|---|---|
| Launch / deploy (Android, physical iPhone), permission dialog, ADB diagnosis | `/launch-app` |
| Drive the UI: taps, waits, fiber recipes, navigation traps, known phones | `/drive-app` |
| Pairing, forget-bond ladder | `/re-pair` |
| OTA through the app | `/ota-via-app` |
| BLE symptoms (split-brain, notify failures, flicker) | `/debug-ble` |
