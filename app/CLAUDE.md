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

## Testing Device Without Hardware
Connect to any BLE device with custom services to test UI rendering logic. The app gracefully handles missing descriptors by falling back to UUIDs.

## Autonomous Agent Notes (Claude / MCP)

### BLE Pairing — Ask the User
First-time pairing requires accepting Android system prompts that are too timing-sensitive for autonomous handling:
1. After tapping CONNECT in the app, Android shows a **"Pairing request"** notification in the status bar shade.
2. The user must swipe down → tap **"Pair & connect"** → tap **"Pair"** on the confirmation dialog.
3. All of this must happen before Android times out waiting for user input and drops the connection (`BT_HCI_ERR_REMOTE_USER_TERM_CONN`, disconnect reason 19).

**Rule:** If a device has never been paired, ask the user to watch for and accept the Android pairing prompts themselves. Once paired, subsequent connections complete automatically without any prompts.

### Launching the App
`npx expo run:android` is a blocking command — always run it as a background task. Use `--device Pixel_9_Pro` (the model name, not the ADB IP:port format):
```bash
npx expo run:android --device Pixel_9_Pro
```
Poll `http://localhost:8081/status` until Metro reports `packager-status:running` before trying to interact with the app.

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
