---
paths:
  - "app/hooks/ble-manager.ts"
  - "app/hooks/use-ble-*.ts"
  - "app/services/ble-*.ts"
  - "app/app/**/bluetooth.tsx"
  - "app/components/bluetooth-device-list-item.tsx"
  - "app/components/reconnecting-pulse-card.tsx"
  - "app/patches/**"
  - "app/plugins/withBleForegroundService.js"
---

# App BLE connection: scan, connect, discover, reconnect

Loads when you read the app's BLE connection code. The wire contract shared with the firmware
(metadata blob, MTU/notify limits, connection interval, UUIDs) is `.claude/rules/ble-gatt-contract.md`;
optimistic writes and notifications are `.claude/rules/app-ble-state-writes.md`; iOS differences are
`.claude/rules/app-ios.md`; symptoms are `/debug-ble`.

## Structure

- Singleton `bleManager` in `app/hooks/ble-manager.ts` with state restoration; connect/discovery
  logic in `app/hooks/use-ble-connection.ts` (`connect()`, triggered from
  `app/components/bluetooth-device-list-item.tsx`). Live characteristic monitors are set up in
  `use-ble-connection.ts` and the per-screen `app/hooks/use-scoped-characteristic-monitors.ts`.
- **Android permissions** (`requestPermissions()` in `ble-manager.ts`): API 31+ requests only
  `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT` (declared `neverForLocation`); `ACCESS_FINE_LOCATION` is
  requested only below API 31. (Why the manifest flags needed a local plugin:
  `.claude/rules/app-config-plugins.md`.)
- Debugging: verbose BLE logging is on in dev builds (`if (__DEV__) bleManager.setLogLevel(LogLevel.Verbose)`
  in `app/app/(tabs)/bluetooth.tsx`). The scan's device-name filter is
  `device.localName?.includes("RGB Sunglasses")` — the FIRMWARE's advertised name
  (`CONFIG_BT_DEVICE_NAME`), deliberately NOT renamed alongside the app's "RGB Glasses" label;
  changing it without a matching firmware release makes every device undiscoverable.

## react-native-ble-plx patch

A patch is applied via `app/patches/react-native-ble-plx+3.5.0.patch` — check it before upgrading
the library. Its core fix: the Android native module (`BlePlxModule.java`) calls
`promise.reject(null, errorConverter.toJs(bleError))` on BLE errors; `code` is `@NonNull` in
Kotlin's `Promise.reject`, so passing `null` throws a native `NullPointerException` that crashes the
entire app process (a native crash, not a JS rejection — no JS try/catch can stop it). The patch
replaces `null` with `bleError.errorCode.name()` at every call site. If an upgrade reintroduces the
pattern (`grep -n "reject(null" node_modules/react-native-ble-plx/android/.../BlePlxModule.java`),
reapply the fix and regenerate the patch. The `postinstall` script applies it automatically.

**`patch-package` has two very different invocations — don't confuse them**: bare `npx
patch-package` (no args) _applies_ every patch under `patches/` to a clean `node_modules`.
`npx patch-package <package-name>` _regenerates_ that package's patch by diffing the current
(possibly hand-edited) `node_modules` against a fresh install — a "save", not a "reapply". Running
the regenerate form against an already-patched tree overwrites the patch file with a huge unintended
diff. To extend an existing patch: reinstall a clean copy of the package, apply existing patches
(`npx patch-package`, no args), make the new edit in `node_modules`, then regenerate
(`npx patch-package <package-name>`) and review the diff line by line.

## Scanning

- **Scan must stop before connecting**: `connect()` calls `bleManager.stopDeviceScan()` before
  `connectToDevice()`. A scan running concurrently with `connectToDevice()` could get the connect
  cancelled by the OS/library even though the native link completed — the app thinks it failed while
  the board thinks it's connected and has stopped advertising.
- **Orphaned BLE scans leak Android's scan-client registrations**: `app/app/(tabs)/bluetooth.tsx`'s
  `useFocusEffect` starts scanning via an unawaited async `startBluetoothScan()`, which awaits
  `requestPermissions()` (several native round-trips) before calling `bleManager.startDeviceScan()`.
  If the screen loses focus during that await, the effect's cleanup runs as a no-op (nothing is
  scanning yet) — then the pending promise starts a scan anyway, with no cleanup left to stop it.
  `react-native-ble-plx` does not stop a prior scan when a new one starts, so each orphaned scan
  permanently consumes one of Android's few scan-client slots, eventually producing
  `SCAN_FAILED_APPLICATION_REGISTRATION_FAILED` (error code 6). Fixed with a per-invocation
  **generation token** (`scanGenRef`): the focus effect bumps it on focus and cleanup, each
  `startBluetoothScan(gen)` bails after any `await` (and inside the scan callback) once the counter
  moved on — correctly handling a fast blur→refocus a shared boolean can't. Plus a defensive
  `stopDeviceScan()` at the top of `startBluetoothScan()`, and the failure-retry `setTimeout` handle is
  stored in a ref and `clearTimeout`d in the cleanup. (On iOS the scan also waits for PoweredOn —
  `stateSubRef`, `.claude/rules/app-ios.md`.) Recovering a phone already in this state: root
  `CLAUDE.md` BT recovery ladder.

## Connect is sequenced link → MTU → discover, with NO `refreshGatt` (issue #90)

`connect()` calls `connectToDevice(mac, { timeout: 60000 })` (no `refreshGatt`, no inline
`requestMTU`; 60 s rather than 15 s because a first-time pair must wait for the user or the
`/re-pair` autoresponder to accept Android's pairing dialog), then `requestMTU(247)` as its own
awaited step, then `requestConnectionPriority(High)`, then discovers. Rationale: (1) MTU as a
separate non-fatal step means a slow/failed exchange can't blow the connect timeout, and reads/writes
work at any MTU (only large *notify* payloads need 247). (2) `refreshGatt:'OnConnected'` calls
Android's `BluetoothGatt.refresh()`, which wipes the cache and forces a full re-discovery on
**every** connect — pure overhead when the cache is valid. It was there to survive a firmware GATT
change on a bonded phone, but hardware testing proved it does **not** rescue that case on a
non-compliant OEM stack (the connection hangs regardless), so it was dropped. `connect()` also
retries `connectToDevice` once (force-closing the failed attempt's half-open GATT client first) for
the controller-level first-attempt failure (HCI 0x3E / reason=62) on a just-rebooted bonded board.

## Split-brain triggers (native link up, app state disconnected)

All four leave the board connected (so not advertising) while the app thinks it isn't; the fast
confirmation is the firmware's `bt_state` (`ATT MTU: 23` on `CONNECTED`/`L4` = stale cache).

1. **Stale Android GATT cache after a GATT-changing reflash** — handles shift, reads hit stale handles
   (`GATT_INVALID_HANDLE`) or discovery hangs. Recovery depends on the phone: stock Android honors the
   firmware's Service Changed indication and re-discovers; OxygenOS does not and needs forget +
   `/re-pair`. Full playbook: `/debug-ble` (`references/stale-gatt-cache.md`); per-phone detail:
   `.claude/skills/drive-app/references/phones.md`.
2. **A failed per-item read during discovery can orphan the connection**: discovery reads are wrapped
   in their own try/catch and skip-on-failure, and the outer `catch` in `connect()` explicitly calls
   `bleManager.cancelDeviceConnection()`. Without that, a thrown error leaves the native link up while
   the app state says disconnected — the device can't be found by scanning again until the app
   process is killed.
3. **App reloads**: a **full** JS reload (`mcp__execbro__reload_app` / dev-menu Reload) or a firmware
   reflash/reset while the phone was connected can leave the native link connected at the OS level
   after the app's JS state is wiped. Fix: `adb shell am force-stop <package>`, then relaunch, so the
   OS notices the client process is gone. Don't wait for the device to reappear in a scan — if no
   fresh `connect()` cycle (`Setting up characteristic monitors...`) has run and the Bluetooth tab shows the "Connect over Bluetooth" hero with no
   device for more than a few seconds, force-stop. Fast Refresh (HMR) is not a trigger (it preserves
   module state, including the `bleManager` singleton; not observed to orphan the link as of 2026-07).
4. **Overlapping `connectToDevice()` calls for the same device** (issue #90 follow-up):
   `react-native-ble-plx`'s Android module tracks one pending subscription per device
   (`DisposableMap`, `connectingDevices`, in `BleModule.java`) and `replaceSubscription()`
   unconditionally disposes whatever was stored under that key — so the *first* promise rejects with
   `BleErrorCode.OperationCancelled` ("Operation was cancelled", errorCode 2) via a `doFinally`
   cleanup path while the *second* call's `establishConnection()` actually completes on the real
   `BluetoothGatt`. The firmware sees a normal connection (`bt_conn_info` shows a
   fast interval); the app sees a rejection. `startConnect()` now dedups **synchronously** through
   the context-level `connectPromises` map (keyed by mac, assigned before the first `await`), so a
   second same-tick call — e.g. a `tap()` delivering two `onPress` events — shares the in-flight
   attempt. React state (`isConnecting`) alone is not enough: it updates asynchronously. If you see
   `Operation was cancelled` with no scan error and `bt_conn_info` shows a live link, suspect this
   mechanism before a new bug.

## Auto-reconnect + Android foreground service (issue #124)

An UNEXPECTED disconnect (OS kill, radio loss, board reset — anything firing `onDeviceDisconnected`)
starts an auto-reconnect supervision loop (`startReconnectLoop()` in `use-ble-connection.ts`) instead
of reverting the UI to "Connect". Key semantics, all unit-tested:

- A **timeout-less pending connect** — `connectToDevice(mac, {autoConnect: true})` on Android,
  `connectToDevice(mac, {})` on iOS — adopts the board the moment it advertises again, with **no
  scanning** (scanning is suppressed while a reconnect is pending, and the reconnecting row is pinned
  in the Connect list). Backoff 2/5/10/30 s applies only to attempts that *error*; retries continue
  **indefinitely** while the app is alive. Every 3rd attempt hedges with a direct `{timeout: 60000}`
  connect in case OEM `autoConnect` is flaky.
- **User-initiated disconnects never auto-reconnect** — structurally (`disconnect()` removes the
  listener before cancelling) plus a context-level `intentionalDisconnectRef` any future on-purpose
  drop (e.g. OTA reboot) can set.
- The **"Reconnecting…" button is the cancel affordance** (tap = `cancelReconnect()`). Cancel bumps
  `reconnectGeneration` (context ref) — in-flight pending connects self-abort via generation
  snapshots even if they resolve after cancellation.
- The per-row connect dedup lives in context (`connectPromises`), so a user tap mid-reconnect
  **shares** the loop's in-flight attempt, and the overlapping-connect guard spans row remounts.
- **Android FGS**: while connected, a `connectedDevice`-typed foreground service
  (`app/services/ble-foreground-service.ts`, notifee) keeps the process + GATT link alive in the
  background. It is only ever **started** from the user-initiated connect path (Android 12+ bans
  background FGS starts); reconnects only update the notification text. Stopped on user
  disconnect/cancel. notifee's bundled service is declared `shortService` (≈3 min cap on Android 14) —
  `app/plugins/withBleForegroundService.js` retypes it via `tools:replace`; if the notification ever
  vanishes ~3 min into backgrounding, suspect that plugin regressed. Play requires an FGS declaration
  (`app/docs/play-publishing.md`). A missing notification small icon is a fatal crash on strict
  phones (`.claude/skills/drive-app/references/phones.md`).
- **Foreground verify** (`app/hooks/use-ble-app-state.ts`, mounted in the root layout): on AppState →
  active, if `selectedDevice` is set but `isDeviceConnected()` is false (a disconnect event missed
  while suspended — the iOS case), it runs the disconnect cleanup and starts the reconnect loop.
- Hardware findings from testing this on the OnePlus (backgrounded link survival, OxygenOS
  system-level auto-connect, wedged bonded reconnects): `.claude/skills/drive-app/references/phones.md`.
  Settings' "Unpair" silently no-ops while any client holds a pending connect — the forget procedure
  is in `/re-pair`.
