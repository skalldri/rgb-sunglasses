# RGB Sunglasses App

React Native controller app for a hardware project: slats sunglasses with RGB LEDs across the frame.
The glasses firmware exposes BLE metadata, and the app generates controls from that metadata at runtime.

## Project Status

This app is actively being rebuilt in React Native and expanded with new features.
The current build already supports:

- Bluetooth discovery and connection flow
- Metadata-driven control UI (no hardcoded per-feature screen list)
- Live characteristic monitoring for notifiable values
- Color picker integration for RGB characteristics
- Firmware update over BLE using MCUmgr/SMP

## Why Metadata-Driven UI

The app and firmware are intentionally decoupled.
On connect, the app discovers services, characteristics, and descriptors, then builds UI controls dynamically.
This lets firmware evolve (new services/characteristics) with less app-side coupling.

Current descriptor usage:

- CUD (`0x2901`): human-readable characteristic name
- CPF (`0x2904`): value format hint (boolean, utf8, uint32, custom color)
- CCC (`0x2902`): notification configuration metadata

## App Behavior Overview

1. Scan for BLE devices named `RGB Sunglasses` and include already-connected known devices.
2. Connect and discover GATT services/characteristics/descriptors.
3. Build an in-memory characteristic model and render controls by type.
4. Monitor notifiable characteristics and reflect updates in UI.
5. Support firmware management through MCUmgr modal.

## UI Mapping (Current)

- Boolean CPF -> `Switch`
- UTF-8 CPF -> text input (write on submit)
- UINT32 CPF -> numeric input (little-endian encode on write)
- Custom color CPF (`0xE0`) -> color picker modal
- MCUmgr SMP characteristic -> firmware update modal entry point

## Tech Stack

- Expo + Expo Router
- React Native + TypeScript
- `react-native-ble-plx` for BLE transport
- Custom `McuMgrClient` for SMP protocol handling
- `patch-package` to apply local BLE library patching

## Getting Started

1. Install dependencies:

```bash
npm install
```

2. Start Metro:

```bash
npm run start
```

3. Run a native build:

```bash
npm run android
# or
npm run ios
```

Note: BLE features require a native/dev build. Do not assume Expo Go supports the full BLE workflow.

## Key Files

- `app/(tabs)/bluetooth.tsx`: scanning and device list
- `components/bluetooth-device-list-item.tsx`: connect/disconnect, discovery, monitors
- `app/(tabs)/device-state.tsx`: dynamic device controls
- `app/color-picker-modal.tsx`: RGB write flow
- `app/firmware-update-modal.tsx`: firmware package selection and upload UI
- `services/mcumgr.ts`: SMP protocol + firmware operations
- `context/bluetooth-context.tsx`: global BLE state and characteristic updates

## Documentation

- `docs/architecture.md`
- `docs/firmware-update.md`
- `docs/roadmap.md`
- `docs/testing-roadmap.md`

## Notes

- A `patch-package` patch is committed at `patches/react-native-ble-plx+3.5.0.patch`.
- The patch preserves BLE error codes in Android promise rejections for better error handling.
