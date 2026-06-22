# RGB Sunglasses App - Copilot Instructions

## Project Overview
React Native Expo app for controlling RGB sunglasses via Bluetooth Low Energy (BLE). Enables color customization, animation control, and firmware updates through a mobile interface.

## Architecture

### Core Pattern: BLE GATT Characteristic-Based State Management
The app mirrors BLE GATT characteristics as UI controls. Each characteristic on the device (boolean, uint32, string, or custom color) automatically renders as the appropriate input (Switch, TextInput, or ColorPicker).

**Critical Files:**
- [context/bluetooth-context.tsx](context/bluetooth-context.tsx) - Global BLE state with `BluetoothContextDevice` structure
- [app/(tabs)/device-state.tsx](app/(tabs)/device-state.tsx) - Dynamic UI rendering based on GATT CPF descriptors
- [constants/bluetooth.ts](constants/bluetooth.ts) - BLE GATT CPF format constants and UUID mappings

### Data Flow
1. **Connection**: [components/bluetooth-device-list-item.tsx](components/bluetooth-device-list-item.tsx) discovers services/characteristics and reads CPF descriptors to determine data types
2. **Rendering**: [device-state.tsx](app/(tabs)/device-state.tsx) `renderCharacteristicInput()` switches on `cpfFormat` to render appropriate control
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

### React Native BLE PLX
Singleton `bleManager` in [hooks/use-ble.ts](hooks/use-ble.ts) with state restoration. **Patch applied** via [patches/react-native-ble-plx+3.5.0.patch](patches/react-native-ble-plx+3.5.0.patch) - check patch file before upgrading library.

The patch's core fix: the library's Android native module (`BlePlxModule.java`) calls `promise.reject(null, errorConverter.toJs(bleError))` on BLE operation errors — `code` is `@NonNull`-annotated in Kotlin's `Promise.reject`, so passing `null` throws a secondary native `NullPointerException` that crashes the entire app process (it's a native crash, not a JS promise rejection, so no JS-level try/catch can stop it). The patch replaces `null` with `bleError.errorCode.name()` at every call site. If a future library upgrade reintroduces this pattern (`grep -n "reject(null" node_modules/react-native-ble-plx/android/.../BlePlxModule.java`), reapply the same fix and regenerate the patch (see the `patch-package` note in Known Issues & Quirks below).

### Expo Router (File-Based Routing)
- Tabs: [app/(tabs)/](app/(tabs)/) directory (bluetooth, device-state, explore, index)
- Modals: [color-picker-modal.tsx](app/color-picker-modal.tsx), [firmware-update-modal.tsx](app/firmware-update-modal.tsx)
- Query params for modal communication: `charUuid`, `r`, `g`, `b`

## Development Workflow

### Running the App
```bash
npx expo start
# Then press 'a' for Android or 'i' for iOS
```

### Android Permissions
Android 12+ (API 31+) requires `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, and `ACCESS_FINE_LOCATION`. Permission handling in [hooks/use-ble.ts](hooks/use-ble.ts) `requestPermissions()`.

### Debugging BLE
- Set `bleManager.setLogLevel(LogLevel.Verbose)` in [bluetooth.tsx](app/(tabs)/bluetooth.tsx)
- Device name filter: `device.localName?.includes("RGB Sunglasses")`
- Monitor subscription in `BluetoothDeviceListItem` for live characteristic updates

## Common Patterns

### Adding New Characteristic Types
1. Define CPF format in [constants/bluetooth.ts](constants/bluetooth.ts)
2. Add decoder logic in `device-state.tsx` useEffect (lines 25-58)
3. Add encoder/renderer in `renderCharacteristicInput()` (lines 112-257)
4. Update `pendingXValues` state if user-editable

### State Updates with Optimistic UI
Always follow this pattern (see [device-state.tsx](app/(tabs)/device-state.tsx) lines 96-112):
```typescript
const previousValue = charInfo.value ?? '';
const encoded = btoa(newValue);
setCharUpdateInProgress(charUuid, true);
charInfo.characteristic.writeWithResponse(encoded)
  .then(() => updateCharValue(charUuid, encoded))
  .catch(() => updateCharValue(charUuid, previousValue)) // Revert on error
  .finally(() => setCharUpdateInProgress(charUuid, false));
```

### Color Encoding
RGB colors are uint32 with lower 24 bits as 0xRRGGBB (little-endian). See [color-picker-modal.tsx](app/color-picker-modal.tsx) for HSV↔RGB conversion and [device-state.tsx](app/(tabs)/device-state.tsx) lines 226-243 for encoding.

## Known Issues & Quirks

- **Scan must stop before connecting**: Call `bleManager.stopDeviceScan()` before `connectToDevice()`
- **McuMgr responses are fragmented**: Read multiple notifications until `moreData` flag is false
- **Base64 encoding everywhere**: All BLE characteristic values are base64, even booleans
- **React Native Reanimated**: Required for navigation animations but causes Metro bundler warnings (safe to ignore)
- **Patch package**: `postinstall` script applies BLE PLX patch automatically
- **Stale Android GATT cache after firmware GATT restructuring**: Android persists a handle-based attribute cache per bonded device. Adding/removing a BLE service or characteristic in firmware shifts attribute handles for everything declared afterward in the GATT database, so a previously-bonded phone can read descriptors by the wrong (now-stale) handle, failing with `GATT_INVALID_HANDLE`. `connectToDevice()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) passes `{ refreshGatt: 'OnConnected' }`, which makes react-native-ble-plx call `BluetoothGatt.refresh()` (Android-only) before discovery, forcing a fresh read instead of trusting the cache. Keep this option set — without it, every firmware GATT change requires the tester to manually forget/re-pair the device on their phone.
- **A failed per-item BLE read during discovery can orphan the connection**: in `connect()`'s discovery loop, descriptor/characteristic reads are wrapped in their own try/catch and skip-on-failure (rather than letting one bad read abort the whole function) — see [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts). The outer `catch` in `connect()` also explicitly calls `bleManager.cancelDeviceConnection()`. Without that, a thrown error during discovery leaves the native BLE link connected at the OS level (so the device stops advertising) while the app's state still thinks it's disconnected — the device then can't be found again by scanning, and the only way out is to force-stop the app (or kill its process) so the OS notices the client is gone and drops the link.
- **`patch-package` has two very different invocations — don't confuse them**: bare `npx patch-package` (no args) *applies* every patch file under `patches/` to a clean `node_modules`. `npx patch-package <package-name>` *regenerates* that package's patch file by diffing the current (possibly already-hand-edited) `node_modules` against a fresh install — i.e. it's a "save", not a "reapply". Running the regenerate form against an already-patched tree overwrites the patch file with a huge unintended diff. To extend an existing patch: reinstall a clean copy of the package, apply existing patches (`npx patch-package`, no args), make the new edit directly in `node_modules`, then regenerate (`npx patch-package <package-name>`) and review the diff line-by-line before trusting it.
- **BLE notifications silently fail without a larger MTU**: `connectToDevice()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) passes `requestMTU: 247`. Without it, the connection stays at the BLE default ATT_MTU (23 bytes, ~20 usable). Unlike writes/reads (which transparently fragment large values via prepare/execute-write and blob-read), a single `bt_gatt_notify()` call cannot be split across multiple ATT PDUs — the whole value must fit in one MTU-bounded packet. A notifiable characteristic whose value exceeds the negotiated MTU fails *firmware-side only* (a `printk` warning, e.g. `bt_att: No ATT channel for MTU ...`), with no error surfaced to the app — the app just never receives the notification and silently keeps showing the old value. See the matching firmware-side note in `fw/CLAUDE.md` (`bt_service_cpp.h notify()`) — even with this MTU bump, a notifiable characteristic whose *content* can grow past ~244 bytes (e.g. `Glim Selection` if the GLIM file count grows a lot) can still exceed the negotiated MTU and needs either a bigger `requestMTU`, a smaller payload, or an app-level read-after-notify pattern.
- **Initial connection/discovery is slow without a connection-priority bump (issue #41)**: the discovery loop in `connect()` does ~170+ sequential GATT reads (one `descriptorsForCharacteristic`/`descriptor.read()`/`characteristic.read()` round-trip per characteristic — can't be parallelized, Android only allows one outstanding GATT operation per connection at a time). Each round-trip takes roughly one full connection interval, and neither side requests a fast one by default (~30-50ms). `connect()` now calls `deviceConnection.requestConnectionPriority(ConnectionPriority.High)` (from `react-native-ble-plx`) right after `connectToDevice()` resolves, dropping the interval to ~7.5-15ms — roughly a 3-4x cut in discovery time. Android-only effect (no-op on iOS); wrapped in try/catch since it's non-fatal if it fails. The firmware makes a matching request from its side (`bt_conn_le_param_update()` in `fw/src/bluetooth.cpp`, see `fw/CLAUDE.md`) as a belt-and-suspenders fallback in case the app-side request doesn't take effect.

## Testing Device Without Hardware
Connect to any BLE device with custom services to test UI rendering logic. The app gracefully handles missing descriptors by falling back to UUIDs.

## Autonomous Agent Notes (Claude / MCP)

### BLE Pairing — Ask the User
First-time pairing requires accepting Android system prompts that are too timing-sensitive for autonomous handling:
1. After tapping CONNECT in the app, Android shows a **"Pairing request"** notification in the status bar shade.
2. The user must swipe down → tap **"Pair & connect"** → tap **"Pair"** on the confirmation dialog.
3. All of this must happen before Android times out waiting for user input and drops the connection (`BT_HCI_ERR_REMOTE_USER_TERM_CONN`, disconnect reason 19).

**Rule:** If a device has never been paired, ask the user to watch for and accept the Android pairing prompts themselves. Once paired, subsequent connections complete automatically without any prompts.

### ADB Wireless Pairing State Lives on the Phone, Not the Container
`adb devices` showing empty does **not** mean the device was never paired. Wireless debugging pairing (the 6-digit code flow) is remembered by the phone; only the TCP connection itself is container-local and drops on container restart. Don't infer "needs full re-pair" from missing local files like `~/.android/known_devices.xml` — those don't reliably reflect pairing state either. Always try `adb connect <ip:port>` first (ask the user for the device's current IP:port from the Wireless debugging screen if unknown); only walk through the full `adb pair` flow if `adb connect` actually fails.

### Launching the App
`npx expo run:android` is a blocking command — always run it as a background task. Use `--device Pixel_9_Pro` (the model name, not the ADB IP:port format):
```bash
npx expo run:android --device Pixel_9_Pro
```
Poll `http://localhost:8081/status` until Metro reports `packager-status:running` before trying to interact with the app.

**`npx expo run:android`'s own install step can fail right after its own successful build**: even when the Gradle build prints `BUILD SUCCESSFUL`, the subsequent `Installing .../app-debug.apk` step can fail with `CommandError: No development build (com.autom8ed.rgbsunglassesapp) for this project is installed. Install a development build on the target device and try again.` — happened consistently in this session, not a one-off. Work around it by installing manually and launching separately:
```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```
then launch the already-installed dev-client app directly (package id has a `.dev` suffix from `applicationIdSuffix ".dev"` in `android/app/build.gradle`, e.g. `com.autom8ed.rgbsunglassesapp.dev`) — use `android_launch_app` or `adb shell am force-stop <pkg> && adb shell monkey -p <pkg> -c android.intent.category.LAUNCHER 1`. Don't pass `--android` to a separately-running `npx expo start` to reconnect Metro — that flag tries to auto-launch generic Expo Go instead of the custom dev-client app that's actually installed.

### BLE Link Can Get Orphaned by App Reloads, Not Just Discovery Failures
The "failed per-item BLE read during discovery can orphan the connection" entry in Known Issues & Quirks above covers one trigger. A second, distinct trigger hit repeatedly in this session: reloading the app mid-session (`mcp__execbro__reload_app`, or a firmware-side J-Link reflash/reset while the phone was connected) can leave the **native BLE link** connected at the OS level even though the app's own JS state has been wiped — the device then stops advertising and can't be found by a fresh scan, no matter how long you wait. The fix is the same as the discovery-failure case: `adb shell am force-stop <package>` (then relaunch) so the OS notices the client process is gone and drops the link. Don't waste time waiting longer for the device to reappear in a scan — if `Setting up characteristic monitors...`/a fresh `connect()` cycle hasn't run and the Bluetooth tab is stuck on "Connect to the RGB Sunglasses" with no device listed for more than a few seconds, force-stop immediately.

### MCP Coordinate Systems
Three coordinate spaces exist and are NOT interchangeable:

| Tool / context | Space | Dimensions |
|---|---|---|
| `android_screenshot()` delivered image | **screenshot px** | 896 × 2000 |
| `tap(x, y)` | **screenshot px** | same — pass coords directly from screenshot |
| `inspect_at_point(x, y)` | **dp** (logical pixels) | ~427 × 953 |
| ADB `input tap` / `native=true` | **raw device px** | 960 × 2142 |

**Converting screenshot px → dp** (needed for `inspect_at_point`):
```
dp = (screenshot_px × 960/896) / 2.25
   ≈ screenshot_px × 0.476
```
Device density is 360 dpi → pixel ratio = 360/160 = **2.25**.

**Status bar**: 153 screenshot px (68 dp) at the top. App content starts below this. `measureInWindow` dp coordinates are relative to the content area (y=0 is below the status bar).

**Practical rule**: get coordinates from the screenshot for `tap()`. Convert to dp for `inspect_at_point()`. Don't mix them up.

### execbro tap coordinates on the OnePlus 9 Pro (LE2125) — read positions from the rendered image, ignore the pressable list
On this device the `android_screenshot` raw frame is 1080×2412 and is delivered downscaled to 896×2000 (~0.829×). Two gotchas burned several taps in one session:
- The **pressables list** that `android_screenshot` prints (e.g. `<AppButton/> "Connect" frame:(714,709 ...)`) reports coordinates that are *inflated* relative to the delivered image (values exceed the 2000px delivered height) — passing them to `tap(x,y)` lands high/short and misses.
- `tap(..., native=true)` does **not** bypass the scaling here — it still multiplies the input by ~1.206, so native taps with raw coords overshoot off-screen.
- What actually works: **visually read the target's position from the delivered screenshot image and pass those pixel coords to `tap(x, y)` (no `native`)**. The tool multiplies by ~1.206 to hit the real raw pixel, and the crosshair lands at roughly the input coordinate in the displayed image. Fiber/`component=`/`text=` taps also misfired (same conversion bug), so coordinate taps read from the image are the reliable path. If a tap shows `meaningful:false`/no change, nudge using the image, don't trust the pressable-list numbers.

### Toggling Switch (Boolean) Characteristics
`Switch` components use `onValueChange`, not `onPress`, so they cannot be triggered via `tap()` by component name or coordinates. Instead, walk the React fiber tree and call the component's `onWrite` prop directly:

```javascript
// In execute_in_app:
(function() {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var fiberRoots = hook.getFiberRoots(1);
  var firstRoot = null;
  fiberRoots.forEach(function(r) { if (!firstRoot) firstRoot = r; });

  var target = null;
  var queue = [firstRoot.current];
  while (queue.length > 0) {
    var fiber = queue.shift();
    if (!fiber) continue;
    var name = fiber.type && (fiber.type.displayName || fiber.type.name || '');
    if (name === 'CharacteristicBoolean') {
      var props = fiber.memoizedProps || {};
      if (props.charUuid === 'TARGET-UUID-HERE') { target = props; break; }
    }
    if (fiber.child) queue.push(fiber.child);
    if (fiber.sibling) queue.push(fiber.sibling);
  }

  // onWrite signature: (charUuid, encodedNewValue, encodedPreviousValue)
  // true  → 'AQ=='  (btoa of byte 0x01)
  // false → 'AA=='  (btoa of byte 0x00)
  target.onWrite('TARGET-UUID-HERE', 'AQ==', target.charInfo.value);
  return 'done';
})()
```

**UUID scheme for animation boolean characteristics:**
- Service UUID: `BT_ANIMATION_SERVICE_UUID(anim_id)` = `12345678-1234-5678-{anim_id<<8:04x}-56789abd0000`
- `Animation::Rainbow = 5` → service `0500`, Is Active (3rd char, index 2) → `12345678-1234-5678-0500-56789abd0002`
- Find current value first: iterate `CharacteristicBoolean` fibers, read `charInfo.value` (`AA==`=false, `AQ==`=true)
- Animation enum values are in `fw/src/animations/animation_types.h`

### BLE Optimistic UI and Notification Behaviour
The app uses optimistic updates: the UI reflects the new value immediately, then reverts if the BLE write returns an error. After a successful write, the **device sends back BLE notifications** with its actual characteristic values. These notifications go through `updateCharValue()` in the Bluetooth context and override the optimistic state with whatever the device actually holds.

Practical implications:
- A write that succeeds in the app may still show a different value if the device sends a notification with a different (e.g., clamped or normalised) value shortly after.
- Characteristic values that are not persisted in NVS reset to firmware defaults after a device reboot.

### Verifying a write/notify round-trip — don't trust a single "it updated" observation
A characteristic whose write-value and notified/stored value differ (e.g. any dropdown-list characteristic, see [components/characteristic-dropdown.tsx](components/characteristic-dropdown.tsx)) is easy to mis-verify, because several distinct bugs all produce the *same* surface symptom: "I picked an option and the UI showed the new value." That observation alone does not distinguish:
- a correct write + correct notify (the real success case),
- an optimistic update that clobbers the real value before the (possibly failed) notify arrives,
- a no-op: the option tapped happened to match what the UI already (possibly stale) believed was selected, so no write was even sent,
- a notify that silently failed (e.g. exceeded the negotiated MTU — see the Known Issues entry above) while the UI happened to already show the right value from a stale read.

What actually caught the MTU/notify bugs in this codebase: reopening the picker afterward to confirm *all* options are still listed (not just the one that appeared selected), and cross-checking the characteristic's value against the firmware's own source of truth immediately after the write (the `glim` shell command, via the `mcp__serial__*` tools) — not a different/unrelated characteristic. When verifying any BLE write, always do both before calling it confirmed.
