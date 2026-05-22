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
