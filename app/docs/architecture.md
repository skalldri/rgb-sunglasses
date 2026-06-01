# Architecture

## Purpose

This app controls RGB LED slats sunglasses over BLE.
The main architecture goal is to keep app and firmware loosely coupled by generating UI from GATT metadata at runtime.

## Design Principles

- Decouple app features from firmware implementation details where possible.
- Build controls from characteristic metadata instead of hardcoding per-device forms.
- Keep BLE session state centralized and persistent across route changes.
- Separate transport/protocol concerns (BLE, SMP) from UI concerns.

## High-Level Runtime Flow

1. User opens Bluetooth tab.
2. App requests platform BLE permissions.
3. App scans for matching devices (`RGB Sunglasses`) and includes already connected devices for known service UUIDs.
4. User connects to a device.
5. App discovers all services, characteristics, and descriptors.
6. App reads descriptor metadata (CUD/CPF/CCC) and initial characteristic values.
7. App stores a normalized device model in context.
8. Device State screen renders controls dynamically from the model.
9. Notifiable characteristics are monitored and update the model reactively.
10. Disconnect cleans subscriptions and clears selected device state.

## Routing Structure

- `app/_layout.tsx`: root providers, theme, stack routes.
- `app/(tabs)/_layout.tsx`: tab navigation.
- `app/(tabs)/bluetooth.tsx`: scanning and device discovery.
- `app/(tabs)/device-state.tsx`: dynamic control rendering.
- `app/color-picker-modal.tsx`: custom color input.
- `app/firmware-update-modal.tsx`: MCUmgr firmware workflow.

## Core State Model

Defined in `context/bluetooth-context.tsx`.

- `selectedDevice`: currently connected device plus discovered metadata.
- `isScanning`: scan lifecycle state.
- `characteristicsByService`: nested map of service UUID -> characteristic UUID -> `CharacteristicInfo`.
- `monitorSubscriptions`: characteristic notification subscriptions.
- `disconnectSubscription`: disconnect listener cleanup handle.

`CharacteristicInfo` currently includes:

- `characteristic`: BLE characteristic object from `react-native-ble-plx`
- `value`: base64 encoded current value
- `name`: user-friendly name from CUD descriptor
- `cpfFormat`: parsed value format from CPF descriptor
- `isUpdateInProgress`: write-in-flight state used by UI

## Metadata-Driven UI

The Device State screen chooses controls by CPF format:

- `BLE_GATT_CPF_FORMAT_BOOLEAN` -> `Switch`
- `BLE_GATT_CPF_FORMAT_UTF8S` -> text input
- `BLE_GATT_CPF_FORMAT_UINT32` -> numeric input
- `BLE_GATT_CPF_FORMAT_CUSTOM_COLOR (0xE0)` -> color picker modal

Additional behavior:

- Service and characteristic names are resolved through constants maps plus descriptor data.
- Characteristic writes update local state on success and revert UI on failure.
- Write success/error is surfaced with a short animated color feedback.

## BLE Integration Notes

- BLE manager is created in `hooks/use-ble.ts` with restored state callback support.
- Android permission logic handles API level `<31` and `>=31` separately.
- Connection flow in `components/bluetooth-device-list-item.tsx` performs:
  - connect
  - discover
  - descriptor read
  - initial characteristic read
  - monitor registration for notifiable characteristics
- MCUmgr characteristic monitoring is excluded from generic monitors because it has dedicated protocol handling.

## Firmware Subsystem Boundary

`services/mcumgr.ts` encapsulates SMP protocol framing, CBOR encoding/decoding, fragmentation, response assembly, and image management commands.
UI code in `app/firmware-update-modal.tsx` only orchestrates user flow and displays status/progress.

## Known Technical Decisions

- `patch-package` is used to patch `react-native-ble-plx` on Android so rejected promises include BLE error code names.
- RGB color writes use uint32 little-endian byte ordering as `[B, G, R, 0]` for `0x00RRGGBB`.
- Dynamic UI generation currently depends on descriptor availability and known CPF values.

## Current Gaps and Future Work

- Improve resilience when descriptors are missing or malformed.
- Expand type handling for additional CPF formats.
- Add stronger test coverage for value encoding/decoding and SMP packet handling.
- Add optional schema/version strategy for firmware-exposed metadata as capabilities grow.
