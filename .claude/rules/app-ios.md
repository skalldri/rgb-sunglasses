---
paths:
  - "app/**/*.ios.tsx"
  - "app/hooks/ble-manager.ts"
  - "app/hooks/use-ble-restoration.ts"
  - "app/hooks/use-ble-app-state.ts"
  - "app/components/characteristic-text-input-base.tsx"
  - "app/scripts/launch-app-ios.sh"
  - "app/plugins/withDevVariantIos.js"
---

# iOS behaviour differences (all hardware-verified)

Loads when you read iOS-sensitive app code. Deploying to a physical iPhone is
`.claude/skills/launch-app/references/ios.md`; the iOS dev variant plugin is
`.claude/rules/app-config-plugins.md`; iOS CI/TestFlight is `.claude/rules/app-release-ci.md`.

## Simulator vs device

**BLE does not work on the iOS Simulator** — it has no Bluetooth radio, so scanning finds nothing.
Simulator verification covers build + UI + navigation only; any live BLE round-trip
(scan/connect/control/firmware-update) needs a **physical iPhone**. The Android-only BLE tuning calls
(`requestConnectionPriority`, `refreshGatt`, `requestMTU`) are no-ops/try-caught on iOS, and
`requestPermissions()` returns `true` on iOS (BLE permission strings come from the
`react-native-ble-plx` Expo plugin config in `app/app.json`, which also sets `UIBackgroundModes`). The
in-app self-update is disabled on iOS entirely (`.claude/rules/app-release-ci.md`).

## BLE on iOS

- **First-time pairing leads discovery (re-verified 2026-08-13, iOS 26.6, post-#232 firmware)**: the
  firmware sends its SMP Security Request from the `connected()` callback, and iOS surfaces the
  passkey pairing dialog ~1 s after connect — BEFORE discovery runs. iOS holds ATT traffic while the
  pairing is pending (passkey entry took ~14 s in testing; zero authentication/encryption errors in
  the whole discovery pass), then discovery runs once, fully encrypted: every characteristic named and
  valued, monitors up, **no manual disconnect+reconnect needed**. This replaces the pre-#232 behaviour
  (full unauthenticated discovery with every read failing `attErrorCode: 5`, dialog only afterwards,
  manual reconnect required — issue #137, now closed). If the user ignores the dialog, SMP times out
  ~30 s in and the connect fails CLEANLY (firmware: `SMP Timeout` → disconnect reason 22; app: connect
  error → row returns to Connect; retry works). The passkey is printed only on the board's serial
  console (`Passkey for <addr>: NNNNNN`) — read it there and enter it on the phone within ~30 s.
- **ATT MTU on iOS is 293** (iPhone 15/iOS 26) — iOS negotiates on its own; `requestMTU(247)` is a
  no-op. Comfortable headroom over Android's 247; zero `bt_att: No ATT channel for MTU` warnings in a
  multi-hour session with all 35 monitors streaming.
- **Discovery is slower on iOS** (~30-55 s vs ~6 s on Android) even with the bulk-metadata path
  working — iOS has no `requestConnectionPriority` equivalent and the firmware's parameter request
  still yields a 15 ms interval, so the gap is likely iOS-side GATT scheduling. Unoptimized as of
  2026-07; measure before assuming regressions.
- **First-launch scan waits for PoweredOn (issue #136, fixed)**: `startBluetoothScan` gates on
  `bleManager.state()` / `onStateChange` before scanning, because CoreBluetooth sits in `Unknown`
  while initializing — and on the very first launch the Bluetooth permission prompt holds it there
  for as long as the user takes to answer (the ungated scan AND its old fixed 2 s retry both died
  with `BluetoothLE is in unknown state`, leaving a dead empty screen until refocus).
  `Unauthorized`/`Unsupported` drop to the empty state instead of waiting forever; `PoweredOff`
  waits, so flipping Bluetooth back on auto-starts the scan. The wait subscription is
  generation-guarded and removed in the focus-effect cleanup (`stateSubRef`).
- **`device.id` is NOT a MAC on iOS**: CoreBluetooth never exposes BLE MAC addresses; ble-plx's
  `device.id` there is an opaque per-phone peripheral UUID (can change if the bond is forgotten).
  Everything keyed off "macAddress" still works (it's just an opaque key), but don't *display* it on
  iOS (`bluetooth-device-list-item.tsx` hides the caption) and don't write iOS logic expecting
  `AA:BB:CC:DD:EE:FF`.

## Text-based parameter inputs commit on `onEndEditing` on iOS

All three (uint32, float32, utf8) go through the shared `app/components/characteristic-text-input-base.tsx`:
iOS's number-pad/decimal-pad keyboards have **no return key**, so `onSubmitEditing` is unreachable —
dismissing the keyboard (tap outside) is the commit signal on iOS, uniformly across field types.
Android is unchanged (✓/Return submits via `onSubmitEditing`, tap-away cancels). Two subtleties in the
base, both regression-tested in `app/__tests__/characteristic-inputs.test.tsx`:

- The no-op-edit skip compares **display strings** (`decodeToDisplay(charInfo.value) === pendingValue`),
  never re-encoded bytes: float32 display is rounded to 7 significant digits (`formatFloat32`), so
  re-encoding it can differ from the stored bytes by 1 ULP — a byte compare turned a casual
  tap-in/tap-out into a BLE write that corrupted the stored value.
- A `submittedRef` suppresses the blur-after-submit double-fire when a return key IS available (iOS
  text keyboard, hardware keyboards) — without it one commit sends two BLE writes.

Decided against an `InputAccessoryView` "Done" bar (rejected in review: extra chrome) and against the
`numbers-and-punctuation` keyboard (a full keyboard for a number field).

## Core Bluetooth state restoration re-adoption (issue #190)

When iOS jetsams the app while a board is connected, Core Bluetooth relaunches it in the background
on the next BLE event for that peripheral. The restore callback (`restoreStateFunction` in
`app/hooks/ble-manager.ts`) is registered at module-import time but **delivered asynchronously** (a
native bridge event that can land before or after React mounts), so the handoff is a
stash-or-deliver, deliver-once subscription (`subscribeRestoredPeripheral`), not a read-once peek.
`useBleRestorationAdopt` (`app/hooks/use-ble-restoration.ts`, mounted as `BleRestorationAdopter` in the
root layout inside `BluetoothProvider`) receives it whichever ordering wins and drives the issue-#124
`startReconnectLoop` (once-only — a duplicate start would bump the reconnect generation and tear down
the restored link). The iOS pending connect resolves immediately on the still-connected peripheral,
then the normal discovery/monitor/selection path runs (fast: iOS serves it from its native GATT cache
on a live link).

Two deliberate limits: **restoration never fires after a user force-quit** (App Switcher swipe) — iOS
only relaunches after a *system* termination (platform limitation) — and **ordinary cold launches do
not auto-connect** (no last-device persistence; scope decision on #190). Hardware-verified 2026-07-18
(iPhone 15/iOS 26.5): SIGKILL of the backgrounded app → relaunch within seconds on the board's next
notify, restore→re-adopt→reconnected on attempt 1, board never saw a disconnect (stayed
CONNECTED/L4/MTU 293), background Metro bundle fetch worked, app→device write round-trip confirmed via
serial. One surprise not to misread: ~50 s **after a user force-quit**, iOS's SYSTEM stack briefly
reconnected to the bonded board (serial showed a bonded L4 connection with NO app process alive) and
dropped it ~15 s later — the iOS analog of OxygenOS's system-level auto-connect. A bonded L4 connection
on serial does not by itself mean the app is running or was relaunched.
