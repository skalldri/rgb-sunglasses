# RGB Sunglasses App - Copilot Instructions

## Project Overview

React Native Expo app for controlling RGB sunglasses via Bluetooth Low Energy (BLE). Enables color customization, animation control, and firmware updates through a mobile interface.

## Architecture

### Core Pattern: BLE GATT Characteristic-Based State Management

The app mirrors BLE GATT characteristics as UI controls. Each characteristic on the device (boolean, uint32, string, or custom color) automatically renders as the appropriate input (Switch, TextInput, or ColorPicker).

**Critical Files:**

- [context/bluetooth-context.tsx](context/bluetooth-context.tsx) - Global BLE state with `BluetoothContextDevice` structure
- [app/(tabs)/device-state/](<app/(tabs)/device-state/>) - a **directory**, not a single file: `index.tsx` is the Controls menu (service list), `[serviceUuid].tsx` is the per-service detail screen
- [hooks/use-characteristic-editor.tsx](hooks/use-characteristic-editor.tsx) - the characteristic dispatch/decode logic: `renderCharacteristicInput()` switches on CPF format to pick a control, `decodeValueForInput()` decodes base64 values per format
- [constants/bluetooth.ts](constants/bluetooth.ts) - BLE GATT CPF format constants and UUID mappings

### Data Flow

1. **Connection**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) (triggered from [components/bluetooth-device-list-item.tsx](components/bluetooth-device-list-item.tsx)) discovers services/characteristics and reads CPF descriptors to determine data types
2. **Rendering**: `renderCharacteristicInput()` in [hooks/use-characteristic-editor.tsx](hooks/use-characteristic-editor.tsx) switches on `cpfFormat` to render appropriate control (a `Characteristic*` component from [components/](components/))
3. **Updates**: Write operations use `writeWithResponse()`, with optimistic updates reverted on error
4. **Encoding**: All BLE values are base64-encoded (`btoa`/`atob`). Numbers use little-endian byte order

### Custom Extensions

- `BLE_GATT_CPF_FORMAT_CUSTOM_COLOR` (0xE0): Non-standard format for RGB color as uint32 (lower 24 bits)
- Custom service UUIDs starting with `12345678-1234-5678-000X-` for different animation services

## Key Technologies

### MCU Manager (MCUmgr)

[services/mcumgr.ts](services/mcumgr.ts) implements SMP (Simple Management Protocol) for Zephyr RTOS firmware updates:

- CBOR-encoded messages with 8-byte headers
- Chunked image uploads (max 256 bytes per write)
- Multi-stage process: upload → test → confirm → reset
- **Important**: Uses sequence numbers and fragmented responses across multiple BLE notifications

### GitHub-releases auto-update check ([firmware-update-modal.tsx](app/firmware-update-modal.tsx))

On mount (once an MCUmgr client connects), the modal sequentially: (1) calls `client.getOsInfo('i')` (OS Management group, `OsCmd.INFO`, added to [services/mcumgr.ts](services/mcumgr.ts) for this feature) to read the device's board name string (e.g. `rgb_sunglasses_proto0_nrf5340_cpuapp`), (2) derives `'proto0' | 'dk' | null` from it via `extractBoardRevision()`, then (3) calls `fetchLatestRelease('skalldri', 'rgb-sunglasses')` (GitHub REST API, no auth — [services/github-releases.ts](services/github-releases.ts)) and picks the release asset whose filename contains the board revision (`findAssetForBoard()`). The found asset's tag (`vX.Y.Z` → stripped via `parseVersionFromTag()`) is compared against the currently-active image's `version` (from `getImageState()`, slot 0 + `active`) via `compareVersions()`; a strictly-older device version surfaces an "Update Available" card with a **Download Update** button. That button downloads the release zip via `expo-file-system/legacy`'s `createDownloadResumable` (not the newer `expo-file-system/next` `File` API used elsewhere in this file — `next` has no resumable-download primitive yet) straight into the same `parseFirmwarePackageFromBase64()` → `firmwarePackage` state the manual `.zip`-picker path already populates, so the existing upload/test/confirm flow (`handleStartUpdate`) is unmodified and shared by both paths.

This was originally built and verified on the pre-monorepo standalone app repo (`skalldri/rgb-sunglasses-app`, `auto-update` branch) and silently never made it across during the monorepo migration — ported back into this repo by re-deriving the diff from that branch rather than re-implementing from scratch. The GitHub release lookup is unauthenticated and rate-limited per-IP by GitHub (60 req/hr) — fine for manual on-demand checks from a single device, but don't add polling/retry-on-mount behavior without adding a token.

### React Native BLE PLX

Singleton `bleManager` in [hooks/ble-manager.ts](hooks/ble-manager.ts) with state restoration (connect/discovery logic lives in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts)). **Patch applied** via [patches/react-native-ble-plx+3.5.0.patch](patches/react-native-ble-plx+3.5.0.patch) - check patch file before upgrading library.

The patch's core fix: the library's Android native module (`BlePlxModule.java`) calls `promise.reject(null, errorConverter.toJs(bleError))` on BLE operation errors — `code` is `@NonNull`-annotated in Kotlin's `Promise.reject`, so passing `null` throws a secondary native `NullPointerException` that crashes the entire app process (it's a native crash, not a JS promise rejection, so no JS-level try/catch can stop it). The patch replaces `null` with `bleError.errorCode.name()` at every call site. If a future library upgrade reintroduces this pattern (`grep -n "reject(null" node_modules/react-native-ble-plx/android/.../BlePlxModule.java`), reapply the same fix and regenerate the patch (see the `patch-package` note in Known Issues & Quirks below).

### Expo Router (File-Based Routing)

- Tabs: [app/(tabs)/](<app/(tabs)/>) directory (bluetooth, device-state/ — itself a directory with `index.tsx` + `[serviceUuid].tsx`, index)
- Modals: [color-picker-modal.tsx](app/color-picker-modal.tsx), [firmware-update-modal.tsx](app/firmware-update-modal.tsx)
- Query params for modal communication: `charUuid`, `r`, `g`, `b`

## Development Workflow

### Running the App

```bash
npx expo start
# Then press 'a' for Android or 'i' for iOS
```

### iOS (macOS) builds

iOS cannot be built in the Linux devcontainer (needs macOS/Xcode) — build it on a Mac (e.g. the
Mac Mini M1). One-time setup: `app/scripts/macos-setup.sh` (idempotent; installs Xcode CLT check +
Homebrew packages, runs `npm ci`, then `expo prebuild --platform ios`). Then `npm run ios`.

The app is a managed Expo project, so `ios/` is generated by `expo prebuild` and gitignored. The
generated Xcode **workspace and scheme are both `RGBSunglasses`** (derived from the Expo `name`
"RGB Sunglasses"), **not** the slug/scheme `rgbsunglassesapp`. The iOS `bundleIdentifier` is
`com.autom8ed.rgbsunglassesapp` (same string as the Android package). CI builds the unsigned
simulator binary on a self-hosted macOS runner via `.github/workflows/app-ios-ci.yml` with
`CODE_SIGNING_ALLOWED=NO` — no Apple Developer account or signing secrets. That workflow triggers
**only on `push` + `workflow_dispatch`, never `pull_request`**: the repo is public and a fork PR runs
the fork's own copy of the workflow, so an in-file `if:` guard is not a security boundary — push-only
means fork code can never reach the self-hosted Mac (forks can't push or dispatch). Per-PR `test` +
Android coverage stays in `app-ci.yml` on GitHub-hosted runners.

**BLE does not work on the iOS Simulator** — the simulator has no Bluetooth radio, so scanning
finds nothing. Simulator verification covers build + UI + navigation only; any live BLE round-trip
(scan/connect/control/firmware-update) needs a **physical iPhone**. The Android-only BLE tuning
calls (`requestConnectionPriority`, `refreshGatt`, `requestMTU`) are already no-ops/try-caught on
iOS, and `requestPermissions()` returns `true` on iOS (BLE permission strings come from the
`react-native-ble-plx` Expo plugin config in `app.json`, which also sets `UIBackgroundModes`).

**TestFlight releases**: the `ios-testflight` job in `.github/workflows/app-release.yml` builds a
signed Release archive and uploads it to TestFlight. The job is gated on the
`TESTFLIGHT_PUBLISH_ENABLED` repo variable — it must be `true` or the job is skipped (a pause
switch that avoids editing the workflow). It runs on the same self-hosted Mac runner,
triggered by `app-vX.Y.Z` tags (in parallel with the Android release job — neither gates the other)
or by `workflow_dispatch` with explicit `version` + `build_number` inputs (iOS-only; the Android
job is skipped — used for the first upload and pipeline validation). Signing is **cloud signing**:
automatic signing + `xcodebuild -allowProvisioningUpdates` authenticated by an App Store Connect
API key (secrets `ASC_API_KEY_P8` base64 / `ASC_KEY_ID` / `ASC_ISSUER_ID`; the non-sensitive Team
ID is the repo *variable* `APPLE_TEAM_ID`) — no distribution .p12 or keychain setup on the runner.
Upload is a second `xcodebuild -exportArchive` with `destination: upload` in the exportOptions
plist (`altool` is deprecated). `ios.buildNumber` is injected at build time by a shared `version`
job (single source of truth — the same value is the Android versionCode): tag builds use
`MAJOR*10000 + MINOR*100 + PATCH` (≥ 10000; the job rejects 0.x.y tags), manual dispatch builds
must be 1–9999 (enforced, so the ranges can't collide). **TestFlight permanently consumes each
(version, buildNumber) pair** — never re-push an
app tag whose TestFlight upload succeeded; bump patch instead (see the release skill). Export
compliance is pre-answered by `ios.infoPlist.ITSAppUsesNonExemptEncryption: false` in `app.json`
(the app uses only standard OS encryption), so builds go straight to internal testers after Apple's
~5–30 min processing. `app-ios-ci.yml` (unsigned simulator build) is unchanged and remains the
per-push check. Optionally, `ios.appleTeamId` can be added to `app.json` for local
`expo run:ios --device` convenience — CI doesn't need it (the workflow passes `DEVELOPMENT_TEAM`
explicitly).

The in-app self-update flow is **disabled on iOS entirely** (it side-loads an APK, which only Android
can do) **and in Google Play builds** (Play policy forbids self-updating outside Play). The
`APP_SELF_UPDATE_SUPPORTED` flag in `services/app-update.ts` (Android + non-null Expo config +
`extra.distribution !== 'play'`; fails closed when the config is unavailable) gates every entry
point: no launch-time check or "update available" banner, the footer shows a plain version label
instead of a "Check for updates" link, and the update modal renders a "not available" message if
reached via deep link. On iOS the app is updated through the App Store / TestFlight; Play builds
are updated by Play itself. The Play variant is produced by the `play` job in
`.github/workflows/app-release.yml` via the shared `build-signed-android` composite action, which
stamps `extra.distribution = "play"` into `app.json` and strips `REQUEST_INSTALL_PACKAGES` — see
[docs/play-publishing.md](docs/play-publishing.md) for the full pipeline, signing-lineage rules
(the CI keystore is the Play app signing key — never rotate it casually), and the one-time Play
Console bootstrap.

There is currently **no iOS dev-variant** (the Android `.dev` side-by-side install from
`plugins/withDevVariant.js` is Android-only); the iOS dev build uses the production bundle id.

### Physical-iPhone dev builds + BLE (verified 2026-07-11, iPhone 15 / iOS 26.5, Xcode 26.2)

**Deploying a local dev build to a physical iPhone** (`npx expo run:ios --device <UDID>`):

- Pass the **traditional hardware UDID** from `xcrun xctrace list devices` (`00008120-…`), NOT the
  CoreDevice UUID that `xcrun devicectl list devices` prints — Expo CLI doesn't match the latter.
  (Expo also warns `Unexpected devicectl JSON version` on Xcode 26; device matching by UDID still works.)
- Expo refuses to build with "No code signing certificates are available" and will NOT mint one.
  First-time bootstrap: sign into Xcode → Settings → Accounts, make sure `ios.appleTeamId` is in
  `app.json` (prebuild bakes it into the project as `DEVELOPMENT_TEAM` — without it xcodebuild fails
  with "requires a development team"), then run
  `xcodebuild -workspace ios/RGBSunglasses.xcworkspace -scheme RGBSunglasses -configuration Debug
  -destination 'id=<UDID>' -allowProvisioningUpdates build` once — that creates the Apple Development
  cert + device-registered profile, after which `expo run:ios --device` works normally.
- Expo's auto-launch after install can silently no-op on physical devices; launch explicitly with
  `xcrun devicectl device process launch --device <UDID> com.autom8ed.rgbsunglassesapp`.
- **Local Network permission**: the app can't reach Metro (LAN IP in the app's `ip.txt`) until the
  user accepts iOS's local-network prompt on first launch — the symptom is "app launches, Metro never
  receives a bundle request". Phone and Mac must share the Wi-Fi network.
- execbro works against a physical iPhone at the **JS level only** (Metro CDP: `scan_metro`,
  `get_logs`, `execute_in_app`); screenshots/tap drivers are simulator-only.
- The board's serial shell from macOS: `/dev/cu.usbmodem*` at 115200 (interface 0 of 2fe3:0001 =
  lower-numbered node). `scripts/fw-shell.sh` is Linux-only (sysfs), and `scripts/hw-lock.sh` needs
  bash ≥ 4 so it does not run on macOS stock bash — when the board is physically attached to the Mac,
  devcontainer agents can't reach it anyway.

**iOS BLE behavior differences (all hardware-verified):**

- **First-time pairing is lazy and races discovery**: iOS does NOT pair during `connectToDevice()`.
  The app's discovery loop runs to completion on the unencrypted link with every read failing
  `attErrorCode: 5` ("Authentication is insufficient", `errorCode: 402`) — services render as bare
  UUIDs — and only afterwards does CoreBluetooth surface the passkey pairing dialog. After the user
  enters the board's passkey (bonded from then on), a **disconnect + reconnect** is needed for a
  clean, fully-named discovery pass; security elevates instantly on the bonded reconnect. Nothing is
  wrong firmware-side when this happens (`bt_state` shows L1 during the race, L4 after re-pair).
- **ATT MTU on iOS is 293** (iPhone 15/iOS 26) — iOS negotiates on its own; `requestMTU(247)` is a
  no-op there. Comfortable headroom over Android's 247; zero `bt_att: No ATT channel for MTU`
  warnings in a multi-hour session with all 35 monitors streaming.
- **Discovery is slower on iOS** (~30-55 s vs ~6 s on Android) even with the bulk-metadata path
  working (it does work, verified) — iOS has no `requestConnectionPriority` equivalent and the
  firmware's `bt_conn_le_param_update()` still yields a 15 ms interval, so the gap is likely
  iOS-side GATT scheduling. Unoptimized as of 2026-07; measure before assuming regressions.
- **First-launch scan fails with `BleError: BluetoothLE is in unknown state`**: the Bluetooth tab
  starts scanning while CoreBluetooth is still initializing / the Bluetooth permission prompt is up.
  It recovers on the next tab focus; a state-aware scan start (wait for PoweredOn) would fix it
  properly.
- **`device.id` is NOT a MAC on iOS**: CoreBluetooth never exposes BLE MAC addresses; ble-plx's
  `device.id` there is an opaque per-phone peripheral UUID (can change if the bond is forgotten).
  Everything keyed off "macAddress" in the app still works (it's just an opaque key), but don't
  *display* it on iOS (`bluetooth-device-list-item.tsx` hides the caption there) and don't write
  iOS logic that expects `AA:BB:CC:DD:EE:FF` format.
- **Text-based parameter inputs commit on `onEndEditing` on iOS** — all three (uint32, float32,
  utf8) via the shared `characteristic-text-input-base.tsx`: iOS's number-pad/decimal-pad keyboards
  have **no return key**, so `onSubmitEditing` is unreachable there — dismissing the keyboard (tap
  outside the field) is the commit signal on iOS, uniformly across field types. Android is unchanged
  (✓/Return submits via `onSubmitEditing`, tap-away cancels). Two subtleties baked into the base,
  both regression-tested in `__tests__/characteristic-inputs.test.tsx`:
  - The no-op-edit skip compares **display strings** (`decodeToDisplay(charInfo.value) ===
    pendingValue`), never re-encoded bytes: float32 display is rounded to 7 significant digits
    (`formatFloat32`), so re-encoding it can differ from the stored bytes by 1 ULP — a byte compare
    turned a casual tap-in/tap-out into a BLE write that corrupted the stored value.
  - A `submittedRef` suppresses the blur-after-submit double-fire when a return key IS available
    (iOS text keyboard, hardware keyboards) — without it one commit sends two BLE writes.
  Decided against an `InputAccessoryView` "Done" bar (rejected in review: extra chrome) and against
  `numbers-and-punctuation` keyboard (full keyboard for a number field).

### Android Permissions

Android 12+ (API 31+) requires `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, and `ACCESS_FINE_LOCATION`. Permission handling in [hooks/ble-manager.ts](hooks/ble-manager.ts) `requestPermissions()`.

### Debugging BLE

- Verbose BLE logging is already on in dev builds: `if (__DEV__) bleManager.setLogLevel(LogLevel.Verbose)` in [bluetooth.tsx](<app/(tabs)/bluetooth.tsx>)
- Device name filter: `device.localName?.includes("RGB Sunglasses")`
- Monitor subscription in `BluetoothDeviceListItem` for live characteristic updates

## Common Patterns

### Adding New Characteristic Types

Follow the `/add-gatt-characteristic` skill (`.claude/skills/add-gatt-characteristic/`, app steps in its `references/app-side.md`) — do not improvise. The files involved:

1. CPF format constant: [constants/bluetooth.ts](constants/bluetooth.ts)
2. Decode + dispatch: [hooks/use-characteristic-editor.tsx](hooks/use-characteristic-editor.tsx) — `decodeValueForInput()` (decode), `renderCharacteristicInput()` (per-format control dispatch), `pendingValues` state for user-editable inputs
3. Encode helpers: [services/ble-value-codec.ts](services/ble-value-codec.ts)
4. The per-format UI component: `components/characteristic-*.tsx`

### State Updates with Optimistic UI

Always follow this pattern (the real implementation is `writeToCharacteristic`/`writeServiceCharacteristic` in [context/bluetooth-context.tsx](context/bluetooth-context.tsx) — see "BLE Optimistic UI and Notification Behaviour" below for the exact ordering rules):

```typescript
const previousValue = charInfo.value ?? "";
const encoded = btoa(newValue);
setCharUpdateInProgress(charUuid, true);
charInfo.characteristic
  .writeWithResponse(encoded)
  .then(() => updateCharValue(charUuid, encoded))
  .catch(() => updateCharValue(charUuid, previousValue)) // Revert on error
  .finally(() => setCharUpdateInProgress(charUuid, false));
```

### Color Encoding

RGB colors are uint32 with lower 24 bits as 0xRRGGBB (little-endian, i.e. bytes `b,g,r,0`). Encoding/decoding lives in [services/ble-value-codec.ts](services/ble-value-codec.ts) (`encodeColorToBase64`/`decodeColorFromBase64`); HSV↔RGB conversion in [color-picker-modal.tsx](app/color-picker-modal.tsx).

## Known Issues & Quirks

- **Scan must stop before connecting**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) calls `bleManager.stopDeviceScan()` before `connectToDevice()` (previously documented here but not actually enforced in code — a scan running concurrently with `connectToDevice()` could get the connect operation itself cancelled by the OS/library even though the native link completed, leaving the app thinking it failed while the board thinks it's connected and has stopped advertising).
- **Orphaned BLE scans leak Android's scan-client registrations**: [app/(tabs)/bluetooth.tsx](<app/(tabs)/bluetooth.tsx>)'s `useFocusEffect` starts scanning via an unawaited async `startBluetoothScan()`, which itself awaits `requestPermissions()` (several native round-trips) before actually calling `bleManager.startDeviceScan()`. If the screen loses focus during that await, the effect's synchronous cleanup runs as a no-op (nothing is scanning yet) — then the pending promise resolves and starts a scan anyway, with no cleanup left to ever stop it. `react-native-ble-plx` does not stop a prior scan when a new one starts (both the JS layer and the Android native module just overwrite the internal subscription), so each orphaned scan permanently consumes one of Android's small number of concurrent scan-client slots, eventually producing `SCAN_FAILED_APPLICATION_REGISTRATION_FAILED` (error code 6). Fixed with a per-invocation **generation token** (`scanGenRef`): the focus effect bumps it on both focus and cleanup, each `startBluetoothScan(gen)` captures its value and bails after any `await` (and inside the scan callback) once the counter has moved on, so only the newest invocation ever touches the scanner — this correctly handles a fast blur→refocus that a shared boolean can't (a refocus would reset the boolean and let a superseded invocation through). Plus a defensive `stopDeviceScan()` at the top of `startBluetoothScan()`, and the failure-retry `setTimeout` handle is stored in a ref and `clearTimeout`d in the cleanup so a pending retry can't fire into the next focus session and start a second concurrent scan.
- **McuMgr responses are fragmented**: Read multiple notifications until `moreData` flag is false
- **Base64 encoding everywhere**: All BLE characteristic values are base64, even booleans
- **React Native Reanimated**: Required for navigation animations but causes Metro bundler warnings (safe to ignore)
- **Patch package**: `postinstall` script applies BLE PLX patch automatically
- **Stale Android GATT cache after firmware GATT restructuring**: Android persists a handle-based attribute cache per bonded device. Adding/removing a BLE service or characteristic in firmware shifts attribute handles for everything declared afterward in the GATT database, so a previously-bonded phone can read descriptors by the wrong (now-stale) handle, failing with `GATT_INVALID_HANDLE`. **`connect()` no longer uses `refreshGatt` to paper over this** — it was tried (`connectToDevice(..., { refreshGatt: 'OnConnected' })`, calling Android's `BluetoothGatt.refresh()` before discovery) but hardware testing proved it does NOT rescue the stale-cache case on a non-compliant OEM stack (the connection hangs regardless) while taxing every healthy connect, so it was dropped. See the two entries below — "Connect is sequenced link → MTU → discover, with NO `refreshGatt`" and the two-phone split-brain writeup — for the current approach and the real recovery (forget + re-pair on a non-compliant stack; stock Android recovers on its own via the firmware's Service Changed indication).
- **A failed per-item BLE read during discovery can orphan the connection**: in `connect()`'s discovery loop, descriptor/characteristic reads are wrapped in their own try/catch and skip-on-failure (rather than letting one bad read abort the whole function) — see [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts). The outer `catch` in `connect()` also explicitly calls `bleManager.cancelDeviceConnection()`. Without that, a thrown error during discovery leaves the native BLE link connected at the OS level (so the device stops advertising) while the app's state still thinks it's disconnected — the device then can't be found again by scanning, and the only way out is to force-stop the app (or kill its process) so the OS notices the client is gone and drops the link.
- **`patch-package` has two very different invocations — don't confuse them**: bare `npx patch-package` (no args) _applies_ every patch file under `patches/` to a clean `node_modules`. `npx patch-package <package-name>` _regenerates_ that package's patch file by diffing the current (possibly already-hand-edited) `node_modules` against a fresh install — i.e. it's a "save", not a "reapply". Running the regenerate form against an already-patched tree overwrites the patch file with a huge unintended diff. To extend an existing patch: reinstall a clean copy of the package, apply existing patches (`npx patch-package`, no args), make the new edit directly in `node_modules`, then regenerate (`npx patch-package <package-name>`) and review the diff line-by-line before trusting it.
- **BLE notifications silently fail without a larger MTU**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) calls `deviceConnection.requestMTU(247)` as its own awaited step after the link is established (not inline in `connectToDevice()` — see the "Connect is sequenced" entry below for why it's separated and non-fatal). Without the bump, the connection stays at the BLE default `ATT_MTU` (23 bytes, ~20 usable). Unlike writes/reads (which transparently fragment large values via prepare/execute-write and blob-read), a single `bt_gatt_notify()` call cannot be split across multiple ATT PDUs — the whole value must fit in one MTU-bounded packet. A notifiable characteristic whose value exceeds the negotiated MTU fails firmware-side only (a `printk` warning, e.g. `bt_att: No ATT channel for MTU ...`), with no error surfaced to the app — the app just never receives the notification and silently keeps showing the old value. See the matching firmware-side note in `fw/CLAUDE.md` (`bt_service_cpp.h notify()`) — even with this MTU bump, a notifiable characteristic whose _content_ can grow past ~244 bytes (e.g. `Glim Selection` if the GLIM file count grows a lot) can still exceed the negotiated MTU and needs either a bigger `requestMTU`, a smaller payload, or an app-level read-after-notify pattern.
- **Initial connection/discovery is slow without a connection-priority bump (issue #41)**: the discovery loop in `connect()` does ~170+ sequential GATT reads (one `descriptorsForCharacteristic`/`descriptor.read()`/`characteristic.read()` round-trip per characteristic — can't be parallelized, Android only allows one outstanding GATT operation per connection at a time). Each round-trip takes roughly one full connection interval, and neither side requests a fast one by default (~30-50ms). `connect()` now calls `deviceConnection.requestConnectionPriority(ConnectionPriority.High)` (from `react-native-ble-plx`) right after `connectToDevice()` resolves, dropping the interval to ~7.5-15ms — roughly a 3-4x cut in discovery time. Android-only effect (no-op on iOS); wrapped in try/catch since it's non-fatal if it fails. The firmware makes a matching request from its side (`bt_conn_le_param_update()` in `fw/src/bluetooth.cpp`, see `fw/CLAUDE.md`) as a belt-and-suspenders fallback in case the app-side request doesn't take effect.
- **Bulk per-service metadata read, cutting discovery further (issue #41 follow-up)**: per service, `connect()` first looks for a characteristic matching `UUID_METADATA_CHARACTERISTIC` (`constants/bluetooth.ts`) — a firmware-synthesized characteristic (see `fw/CLAUDE.md`'s `bt_service_cpp.h` entry) whose value is a packed blob containing every sibling characteristic's CUD name + CPF format. If found, it's read once and parsed via `parseMetadataBlob()` (`services/ble-value-codec.ts`), then zipped _positionally_ onto that service's characteristic list — replacing what would otherwise be 2 descriptor reads (CUD + CPF) per characteristic with 1 read for the whole service. Falls back automatically to the original per-descriptor path on any read failure, blob-version mismatch, or entry-count mismatch (logged, never silently mis-zipped) — this is also what happens for services that don't have the characteristic at all, e.g. the third-party McuMgr service, or any firmware build with `CONFIG_APP_BT_METADATA_CHARACTERISTIC=n` (disabled on `rgb_sunglasses_dk` for flash-size reasons). Extension animation services (`fw/src/extensions/extension_bt.cpp`, issue #90 follow-up) now synthesize this same characteristic at runtime too — this hook needed no changes to pick them up, since it never special-cased built-in vs. extension services in the first place. The positional zip relies on `characteristicsForService()` returning characteristics in firmware GATT declaration order — true by the ATT spec's ascending-handle-order guarantee for characteristic discovery (see the ordering-assumption comment block in `use-ble-connection.ts` and the matching one in `fw/src/bluetooth/bt_service_cpp.h` for the full chain, including the one verified-but-not-enforced link: react-native-ble-plx's Android module passes Android's native discovery order through unmodified). Hardware-verified: total discovery time across all 9 services dropped from ~13-30s to ~6s, with every service correctly using the bulk path and zero fallback/mismatch warnings.

- **A stale Android GATT cache on a bonded device causes the "split-brain" (board LED solid, app times out) — but ONLY on non-spec-compliant OEM stacks; stock Android recovers on its own (issue #90, hardware-proven on two phones)**: Android caches a bonded device's GATT database (handles/services) per bond. Any firmware reflash that changes the GATT layout (adds/removes/reorders a service or characteristic) shifts handles and makes that cache stale. The spec-defined recovery is the firmware's Service Changed indication / Database Hash (`CONFIG_BT_GATT_SERVICE_CHANGED` + `CONFIG_BT_GATT_CACHING`, both on) telling the phone to re-discover. **Whether the phone honors it is device-dependent — verified directly by adding a characteristic, reflashing WITHOUT re-pairing, and watching:**
  - **Pixel 9 Pro (stock Android 16): honors it.** Re-discovered automatically, found the new characteristic, `bt_state` showed a healthy `ATT MTU: 498` link — no re-pair needed. This is correct/spec-compliant behavior and is what end users on stock Android (and presumably most AOSP-derived stacks) will experience across a firmware OTA.
  - **OnePlus 9 Pro (OxygenOS / Android 14): does NOT honor it.** The bonded reconnect **hangs** — link up + encrypted (`bt_state`: `CONNECTED`, `L4`) but stuck at `ATT MTU: 23`, and `requestMTU`/`discoverAllServicesAndCharacteristics` both time out. **No app-side connect option rescues it** — tested `refreshGatt` on/off and `requestMTU` before/after discovery; every combination hangs. The only fix is **Settings → Bluetooth → forget the device, then re-pair** (needs the user for the passkey — see the BLE-Pairing note above), which wipes the stale cache.
  - Practical rule: if the board LED is solid but the app can't discover *after a firmware reflash* and the scanner isn't the problem, it's a stale cache on a non-compliant stack — forget+re-pair. Fastest confirmation: the firmware `bt_state` shell command (see `fw/CLAUDE.md`) — `ATT MTU: 23` on a `CONNECTED`/`L4` link **is** the split-brain. The shared OnePlus dev phone hits this on every GATT-changing reflash; a Pixel does not.
- **Connect is sequenced link → MTU → discover, with NO `refreshGatt` (issue #90)**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) calls `connectToDevice(mac, { timeout: 15000 })` (no `refreshGatt`, no inline `requestMTU`), then `requestMTU(247)` as its own awaited step, then discovers. Rationale: (1) MTU as a separate non-fatal step means a slow/failed exchange can't blow the connect timeout, and reads/writes still work at any MTU (only large *notify* payloads need the 247 bump). (2) `refreshGatt:'OnConnected'` calls Android's `BluetoothGatt.refresh()`, which wipes the on-device cache and forces a full re-discovery on **every** connect — pure overhead when the cache is valid (the normal case). It was originally there to survive a firmware GATT change on a bonded phone, but the stale-cache testing above proved it does **not** help that case (it hangs too), so it bought nothing for the failure mode while taxing every healthy connect — dropped. `connect()` also retries `connectToDevice` once (force-closing the failed attempt's half-open GATT client first) for the controller-level first-attempt failure (HCI 0x3E / reason=62) on a just-rebooted bonded board.

## Testing Device Without Hardware

Connect to any BLE device with custom services to test UI rendering logic. The app gracefully handles missing descriptors by falling back to UUIDs.

## Autonomous Agent Notes (Claude / MCP)

### Device-Free Validation Loop

For any app change that doesn't need the physical phone, run the `/validate-app` skill (`.claude/skills/validate-app/SKILL.md`): `npm ci` in `app/` (reapplies the ble-plx patch via `postinstall`), then jest + typecheck + lint. There is **no `typecheck` npm script** — it's `npx tsc --noEmit` directly. CI (`.github/workflows/app-ci.yml`) runs jest (plus a debug Android build) but neither tsc nor lint, so a green CI run does not mean the types are clean — run them yourself.

### App-Update Modal Auto-Opens After Force-Stop + Relaunch

After `adb shell am force-stop` + relaunch, the in-app self-update check runs on mount and can immediately push the **App Update** modal (`app-update-modal`) on top of the bluetooth tab if a newer release is found on GitHub. This leaves the navigation stack as `__root > (tabs) > app-update-modal > (tabs) > bluetooth`, which blocks tapping anything underneath. Dismiss it with `adb shell input keyevent KEYCODE_BACK` before trying to interact with the Bluetooth or Controls screens. The BLE connection (if triggered before the modal appeared) is still live — the button will show "Disconnect" once the modal is cleared.

### BLE Pairing — prefer the `/re-pair` skill

**Preferred: `scripts/re-pair.sh` / the `/re-pair` skill** automates forget + re-pair
hands-off. It arms a serial watcher on the board's shell UART, taps CONNECT, and a local
autoresponder types the board-displayed passkey into Android's dialog fast enough to beat
the reason-19 timeout — no human in the loop. Requires the `board` + `app` locks and any
MCP serial connection closed first (see the skill). Use it for the OnePlus stale-GATT
split-brain (`/debug-ble`) and any re-pair.

**Manual fallback** (if `/re-pair` can't drive the phone, e.g. Settings-UI drift on the
forget step). First-time pairing accepts Android system prompts that are timing-sensitive:

1. After tapping CONNECT in the app, Android shows a **"Pairing request"** notification in the status bar shade.
2. Swipe down → tap **"Pair & connect"** → tap **"Pair"** on the confirmation dialog, then enter the 6-digit code the board prints on serial (`Passkey for … : NNNNNN`).
3. All of this must happen before Android times out waiting for user input and drops the connection (`BT_HCI_ERR_REMOTE_USER_TERM_CONN`, disconnect reason 19).

If a device has never been paired and `/re-pair` isn't being used, ask the user to watch for and accept the Android pairing prompts themselves. Once paired, subsequent connections complete automatically without any prompts.

### ADB Wireless Pairing State Lives on the Phone, Not the Container

`adb devices` showing empty does **not** mean the device was never paired. Wireless debugging pairing (the 6-digit code flow) is remembered by the phone; only the TCP connection itself is container-local and drops on container restart. Don't infer "needs full re-pair" from missing local files like `~/.android/known_devices.xml` — those don't reliably reflect pairing state either. Always try `adb connect <ip:port>` first (ask the user for the device's current IP:port from the Wireless debugging screen if unknown); only walk through the full `adb pair` flow if `adb connect` actually fails.

### Launching the App

**Hold the `app` hardware lock first — and use `app/scripts/launch-app.sh`, not raw `npx expo run:android`.** There is only one physical phone shared across every agent worktree. `hold` is the only way to take the lock (see root `CLAUDE.md` "Hardware locking" / `.claude/skills/hw-lock/SKILL.md`):

```
Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

`app/scripts/launch-app.sh` no longer acquires this lock itself — it only verifies it's already held and hard-refuses to run otherwise, and refuses to start a second Metro instance even from this same session if you forgot one is already running. The relationship to the lock's own lifetime is asymmetric, not fully independent: stopping the `hold` task (or a same-session `release app --force`) now also stops Metro if it's still running — the script records its own pid against the lock right before exec-ing into Metro, so release can find and stop it precisely — meaning releasing the lock reliably guarantees Metro has quit. It doesn't run in reverse, though: Metro stopping or crashing on its own still does NOT release the lock — you still manage that side yourself. If a Metro/expo process is still running from an earlier, now-dead session when `app` is next held, that hold detects and kills it automatically before considering itself established. A `PreToolUse` hook also auto-denies `mcp__execbro__*` calls and `adb`/`expo run:android` in Bash without the lock. **Never call `npx expo run:android` directly** — doing so bypasses the lock and the single-Metro-instance guarantee, reintroducing exactly the collision risk this exists to prevent.

`app/scripts/launch-app.sh` (which runs `npx expo run:android` internally) is a blocking command — always run it as a background task. Use `--device <device name>` (the model name, not the ADB IP:port format):

```bash
app/scripts/launch-app.sh --device <device name>
```

**Never pass an ADB `ip:port` to `--device`.** Expo CLI matches `--device` against its _own_ device list (model/AVD names), not ADB serials, so a wireless target like `--device 192.168.1.34:41181` fails immediately with `CommandError: Could not find device with name: <ip:port>`. Pass the model name (`Pixel_9_Pro`, `LE2125`, …). With exactly one device attached (check `adb devices`), you can also **omit `--device` entirely** and Expo auto-selects it — handy for a wirelessly-connected phone whose model name you don't have to hand.

Poll `http://localhost:8081/status` until Metro reports `packager-status:running` before trying to interact with the app.

#### Running the app from inside a git worktree — MANDATORY procedure (read before doing anything)

A fresh worktree (`.claude/worktrees/<name>/app`) has **no `node_modules` and no `android/` of its own.** There is exactly one correct way to run the app from it. Follow it verbatim; the tempting shortcuts below are all forbidden because each one has already broken a session.

**DO — the only supported sequence:**

1. `cd <worktree>/app && npm ci` — a **real** install into the worktree. Takes ~30s and reapplies the ble-plx patch via `postinstall`. This is required for `jest`/`tsc` **and** for Metro. Eat this cost.
2. Hold the `app` lock first: `Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)`, then confirm with a short `check app` poll (see "Launching the App" above).
3. `app/scripts/launch-app.sh` — launched as a **harness-managed background task** (Bash `run_in_background: true`). It verifies the `app` lock is held (refuses otherwise), then runs `expo prebuild` (generates `android/`), builds via gradle (fast once the shared gradle cache is warm — ~1 min), starts Metro, installs the APK, and launches the app pointing at its own Metro. Leave it running for the whole session — it owns Metro, but does **not** own the lock; stop the `hold` task separately when you're done with the phone.
4. Poll `http://localhost:8081/status` for `packager-status:running`, then screenshot to confirm the app loaded.

**DON'T — every one of these has caused a failure, do not attempt any of them:**

- **NEVER call `npx expo run:android` directly** — always go through `app/scripts/launch-app.sh`. Calling npx directly bypasses the `app` hardware lock check and the single-Metro-instance guarantee, so a second agent (or a forgotten earlier launch in this same session) can start a colliding second Metro instance against the one physical phone.
- **NEVER symlink `node_modules`** from the main checkout into the worktree (`ln -s <main-checkout>/app/node_modules ...`). The gradle build tolerates it, but **Metro's JS resolver cannot resolve modules through a symlink whose realpath is outside the worktree project root** — you get `UnableToResolveError: Unable to resolve module ./app/node_modules/expo-router/entry` and a red-screen `development server returned response error code: 404` on the device. Always `npm ci` for a real `node_modules`.
- **NEVER background `launch-app.sh` by hand with `&` and/or `> log 2>&1`.** That daemonizes it yourself, the harness sees the wrapper "complete" immediately, loses track of the task, and Metro gets reaped — the app then can't fetch its bundle. Use Bash `run_in_background: true` with the bare command (no `&`, no redirect) so the harness keeps it alive as a tracked task.
- **NEVER substitute `expo start --dev-client` + `adb reverse` + a `rgbsunglassesapp.dev://expo-development-client/?url=...` deep link** to avoid the native build. The installed dev client resumes its stale bundle without re-fetching, the deep link doesn't reliably trigger a fresh bundle against the new Metro, and you burn more time than a build would cost. Also don't pass `--android` to a separate `expo start` (it launches Expo Go, not the dev client).
- **NEVER kill the underlying `expo run:android` process directly** to "restart Metro." If Metro seems wrong, fix the actual cause (usually a stale/symlinked `node_modules`), then stop the `launch-app.sh` background task and relaunch it — this no longer touches the `app` lock either way, since launch-app.sh doesn't own it.

In short: in a worktree, **`npm ci`, then hold `app` via `Monitor`, then `app/scripts/launch-app.sh` as a background task.** No symlinks, no manual daemonizing, no `expo start` deep-link dance. Eat the full build cost — it is cheaper than every workaround.

**Root cause of `CommandError: No development build (com.autom8ed.rgbsunglassesapp) for this project is installed`, and the real fix (not a workaround)**: this project's `plugins/withDevVariant.js` config plugin intentionally injects `applicationIdSuffix ".dev"` into the debug build type (`android/app/build.gradle`) so the debug and release APKs can be installed side-by-side with distinct icons/schemes — the actual installed runtime package id is `com.autom8ed.rgbsunglassesapp.dev`, not the bare `applicationId`. Expo CLI's package-id resolver (`@expo/config-plugins`'s `Package.getApplicationIdAsync()`, called from `AndroidAppIdResolver`) only regexes the literal `applicationId '...'` line out of `build.gradle` — it has no knowledge of per-buildType `applicationIdSuffix`. So `expo run:android` always computes the unsuffixed id, checks whether _that_ is installed (`PlatformManager.openProjectInCustomRuntimeAsync` → `isAppInstalledAndIfSoReturnContainerPathForIOSAsync`), finds it isn't (only the suffixed `.dev` one is), and throws — even immediately after its own build+install step succeeded. This will happen on every `expo run:android` invocation as long as `withDevVariant.js`'s suffix exists, build success or not.

Expo CLI has a built-in flag for exactly this situation — `--app-id <appId>` — which makes it check/install/launch the given id instead of guessing one from `build.gradle`:

```bash
app/scripts/launch-app.sh --device <device name>
```

This is the correct fix to reach for, not the manual `adb install` + `monkey`/`android_launch_app` dance — that manual path still works as a fallback (e.g. if Metro itself won't start), but `--app-id` fixes the actual CLI invocation so it works end-to-end unattended. Don't pass `--android` to a separately-running `npx expo start` to reconnect Metro — that flag tries to auto-launch generic Expo Go instead of the custom dev-client app that's actually installed.

### BLE Link Can Get Orphaned by App Reloads, Not Just Discovery Failures

The "failed per-item BLE read during discovery can orphan the connection" entry in Known Issues & Quirks above covers one trigger. A second, distinct trigger hit repeatedly in this session: reloading the app mid-session (`mcp__execbro__reload_app`, or a firmware-side J-Link reflash/reset while the phone was connected) can leave the **native BLE link** connected at the OS level even though the app's own JS state has been wiped — the device then stops advertising and can't be found by a fresh scan, no matter how long you wait. The fix is the same as the discovery-failure case: `adb shell am force-stop <package>` (then relaunch) so the OS notices the client process is gone and drops the link. Don't waste time waiting longer for the device to reappear in a scan — if `Setting up characteristic monitors...`/a fresh `connect()` cycle hasn't run and the Bluetooth tab is stuck on "Connect to the RGB Sunglasses" with no device listed for more than a few seconds, force-stop immediately. Note that Fast Refresh (HMR) is not one of these triggers: it preserves JS module state (including the module-scope `bleManager` singleton in [hooks/ble-manager.ts](hooks/ble-manager.ts)) and has not been observed to orphan the link (as of 2026-07); the confirmed triggers are a **full** JS reload (`mcp__execbro__reload_app` / dev-menu Reload) or a firmware reflash/reset while connected.

### A third split-brain trigger: overlapping `connectToDevice()` calls for the same device (issue #90 follow-up)

A third distinct trigger for the same native-connected/JS-disconnected split-brain: calling `bleManager.connectToDevice()` a **second** time for the same MAC address while a first call is still pending. `react-native-ble-plx`'s Android native module tracks one pending subscription per device (`DisposableMap`, `connectingDevices`, in `BleModule.java`) and its `replaceSubscription()` **unconditionally disposes whatever was already stored under that key** before storing the new one — so the *first* `connectToDevice()` promise rejects with `BleErrorCode.OperationCancelled` ("Operation was cancelled", errorCode 2) via a `doFinally` cleanup path, while the *second* call's `establishConnection()` is what actually completes on the real `BluetoothGatt`. The firmware sees a normal, successful connection (confirm via `bt_conn_info` — fast connection interval); the app just sees the first call's promise reject.

`connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) now guards against this with a `connectingRef` (a `useRef`, checked and set **synchronously** at the very top of `connect()`, before the first `await`) — `isConnecting` **state** alone isn't sufficient, because state updates are asynchronous: a second `onPress` delivered before the component re-renders sees `isConnecting` still `false` and the button's `disabled={isConnecting}` hasn't taken effect yet, so both calls can reach `bleManager.connectToDevice()` for the same macAddress. Hit exactly this way during testing: a `tap()` on the "Connect" button (via `mcp__execbro__tap`) can deliver more than one `onPress` in the same tick as a fast double-tap would.

If you see `Operation was cancelled` on `connectToDevice()` with no scan-related error in the logs (ruling out the scan-leak class of bug above) and `bt_conn_info` on the firmware shows a genuinely live connection, suspect this mechanism specifically, not a fresh new bug — check whether `connect()` was somehow invoked twice for the same device before concluding otherwise.

### MCP Coordinate Systems

Three coordinate spaces exist and are NOT interchangeable:

| Tool / context                         | Space                   | Dimensions                                  |
| -------------------------------------- | ----------------------- | ------------------------------------------- |
| `android_screenshot()` delivered image | **screenshot px**       | 896 × 2000                                  |
| `tap(x, y)`                            | **screenshot px**       | same — pass coords directly from screenshot |
| `inspect_at_point(x, y)`               | **dp** (logical pixels) | ~427 × 953                                  |
| ADB `input tap` / `native=true`        | **raw device px**       | 960 × 2142                                  |

**Converting screenshot px → dp** (needed for `inspect_at_point`):

```
dp = (screenshot_px × 960/896) / 2.25
   ≈ screenshot_px × 0.476
```

Device density is 360 dpi → pixel ratio = 360/160 = **2.25**.

**Status bar**: 153 screenshot px (68 dp) at the top. App content starts below this. `measureInWindow` dp coordinates are relative to the content area (y=0 is below the status bar).

**Practical rule**: get coordinates from the screenshot for `tap()`. Convert to dp for `inspect_at_point()`. Don't mix them up.

### execbro tapping on the OnePlus 9 Pro (LE2125) — use `strategy="accessibility"` first

**The most reliable approach on this device is `tap(text="...", strategy="accessibility")`.** It fires directly via the Android accessibility tree without any coordinate conversion ambiguity, and it worked in every verified session. Try this first for any button/link with a visible label.

Two things that do **not** work reliably:

- The **pressables list** that `android_screenshot` prints (e.g. `<AppButton/> "Connect" frame:(714,709 ...)`) reports coordinates that are _inflated_ relative to the delivered image — passing them to `tap(x,y)` lands high/short and misses.
- `tap(..., native=true)` and coordinate taps from the screenshot image also misfired repeatedly across multiple sessions. The crosshair appears where the tap landed but the resulting position in the delivered image is inconsistent.

**When `strategy="accessibility"` can't distinguish between two elements with the same label** (e.g. a "Connect" `AppButton` and a "Connect" `BottomTabItem` both matching), use `execute_in_app` to walk the fiber tree and fire `onPress` directly:

```javascript
// Find the first AppButton whose title matches, fire its onPress
(function() {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var roots = hook.getFiberRoots(1);
  var root = null;
  roots.forEach(function(r) { if (!root) root = r; });
  var q = [root.current];
  while (q.length) {
    var f = q.shift();
    if (!f) continue;
    var n = f.type && (f.type.displayName || f.type.name || '');
    if (n === 'AppButton') {
      var p = f.memoizedProps || {};
      if (p.title === 'Connect' && p.onPress) { p.onPress(); return 'fired'; }
    }
    if (f.child) q.push(f.child);
    if (f.sibling) q.push(f.sibling);
  }
  return 'not found';
})()
```

Note: `BaseExpoRouterLink` children are not a bare string in the fiber props — `tap(text=..., strategy="accessibility")` handles those correctly without needing fiber tree surgery.

### Toggling Switch (Boolean) Characteristics

`Switch` components use `onValueChange`, not `onPress`, so they cannot be triggered via `tap()` by component name or coordinates. Instead, walk the React fiber tree and call the component's `onWrite` prop directly:

```javascript
// In execute_in_app:
(function () {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var fiberRoots = hook.getFiberRoots(1);
  var firstRoot = null;
  fiberRoots.forEach(function (r) {
    if (!firstRoot) firstRoot = r;
  });

  var target = null;
  var queue = [firstRoot.current];
  while (queue.length > 0) {
    var fiber = queue.shift();
    if (!fiber) continue;
    var name = fiber.type && (fiber.type.displayName || fiber.type.name || "");
    if (name === "CharacteristicBoolean") {
      var props = fiber.memoizedProps || {};
      if (props.charUuid === "TARGET-UUID-HERE") {
        target = props;
        break;
      }
    }
    if (fiber.child) queue.push(fiber.child);
    if (fiber.sibling) queue.push(fiber.sibling);
  }

  // onWrite signature: (charUuid, encodedNewValue, encodedPreviousValue)
  // true  → 'AQ=='  (btoa of byte 0x01)
  // false → 'AA=='  (btoa of byte 0x00)
  target.onWrite("TARGET-UUID-HERE", "AQ==", target.charInfo.value);
  return "done";
})();
```

**UUID scheme for animation boolean characteristics:**

- Service UUID: `BT_ANIMATION_SERVICE_UUID(anim_id)` = `12345678-1234-5678-{anim_id<<8:04x}-56789abd0000`
- `Animation::Rainbow = 5` → service `0500`, Is Active (3rd char, index 2) → `12345678-1234-5678-0500-56789abd0002`
- Find current value first: iterate `CharacteristicBoolean` fibers, read `charInfo.value` (`AA==`=false, `AQ==`=true)
- Animation enum values are in `fw/src/animations/animation_types.h`
- **Extension animations** (`fw/src/extensions/`, ids `0x40 + slot`) → service groups `4000`, `4100`, … Their "Is Active" uses the FIXED shared UUID `...-bbbb-...0000` — the same literal UUID appears in every animation service, so **always disambiguate by `charInfo.characteristic.serviceUUID`, never by `charUuid` alone**. Their param characteristics use auto UUIDs `...-{group}-56789abd0001/0002/...` in manifest declaration order (ids start at 1).

**Per-CPF fiber component names** (all take the same `onWrite(charUuid, encodedNewValue, encodedPreviousValue)` prop, so the CharacteristicBoolean recipe above works for every type): `CharacteristicBoolean`, `CharacteristicUint32` (4-byte LE, e.g. 50 = `MgAAAA==`), `CharacteristicColor` (4 bytes `b,g,r,0`), `CharacteristicUtf8` (write with `btoa("text")`). The fiber walk matches `fiber.type.displayName || fiber.type.name` — these components are named function exports (`components/characteristic-*.tsx`) with no explicit `displayName`, so it's the function *name* that matches; nothing automated enforces this, so keep those component function names stable as a convention (renaming one silently breaks these recipes). These fibers only exist while the screen that renders them is mounted — Is Active toggles live on the Controls list, per-param characteristics only on that animation's detail page (navigate there first or the walk returns "not found").

### BLE Optimistic UI and Notification Behaviour

The app uses optimistic updates: the UI reflects the new value immediately, then reverts if the BLE write returns an error. The optimistic value is applied **synchronously before `await writeWithResponse(...)`** in `writeToCharacteristic`/`writeServiceCharacteristic` (`context/bluetooth-context.tsx`), batched into the same render as `isUpdateInProgress=true`. On rejection the `catch` reverts to the captured previous value **compare-and-swap style** — only if the current value is still the one we optimistically wrote — so a device notification (or an overlapping write) that landed during the in-flight write isn't clobbered by a stale revert. (It used to run in the write promise's `.then()` — that ordering left a render where a controlled input like the "Is Active" `Switch` still showed its old value while the write was in flight, which caused the toggle-flicker fixed in issue #91.) After a successful write, the **device sends back BLE notifications** with its actual characteristic values. These notifications go through `updateCharValue()` in the Bluetooth context and override the optimistic state with whatever the device actually holds.

Practical implications:

- A write that succeeds in the app may still show a different value if the device sends a notification with a different (e.g., clamped or normalised) value shortly after.
- Characteristic values that are not persisted in NVS reset to firmware defaults after a device reboot.
- **Firmware must refuse unacceptable writes with an ATT error, not "success + corrective notify".** Firmware should reject bad writes with an ATT **error** (triggering the app's catch-and-revert), never accept-then-notify-a-different-value — see the matching rule in `fw/CLAUDE.md` (`bt_service_cpp.h` section). Historically this was mandatory because the optimistic update ran in the write's `.then()` and would clobber any notification that arrived before the write response resolved (a corrective notify was silently lost — hardware-verified). Since the issue-#91 fix moved the optimistic update to *before* the `await`, a notification arriving during the write window now lands after it and wins, so a corrective notify would no longer be silently lost — but rejecting with an ATT error remains the required contract (it's what drives the revert, and it avoids a visible flash-then-correct). Notifications remain the mechanism for device-originated changes that happen after the write completes (e.g. an extension sandbox fault flipping Is Active off).

### Verifying a write/notify round-trip — don't trust a single "it updated" observation

A characteristic whose write-value and notified/stored value differ (e.g. any dropdown-list characteristic, see [components/characteristic-dropdown.tsx](components/characteristic-dropdown.tsx)) is easy to mis-verify, because several distinct bugs all produce the _same_ surface symptom: "I picked an option and the UI showed the new value." That observation alone does not distinguish:

- a correct write + correct notify (the real success case),
- an optimistic update that clobbers the real value before the (possibly failed) notify arrives,
- a no-op: the option tapped happened to match what the UI already (possibly stale) believed was selected, so no write was even sent,
- a notify that silently failed (e.g. exceeded the negotiated MTU — see the Known Issues entry above) while the UI happened to already show the right value from a stale read.

What actually caught the MTU/notify bugs in this codebase: reopening the picker afterward to confirm _all_ options are still listed (not just the one that appeared selected), and cross-checking the characteristic's value against the firmware's own source of truth immediately after the write (the `glim` shell command, via the `mcp__serial__*` tools) — not a different/unrelated characteristic. When verifying any BLE write, always do both before calling it confirmed.
