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
npm run ios   # macOS only — not supported in the devcontainer
```

Note: BLE features require a native/dev build. Do not assume Expo Go supports the full BLE workflow.

## Developing in the Devcontainer (Android)

The repo's devcontainer (`.devcontainer/`) ships the firmware toolchain **and** a React Native
Android toolchain (Node, JDK 17, Android SDK/NDK), so you can build and run the Android app
without leaving the container. BLE only works on a **physical phone** (not an emulator or Expo
Go), so the dev loop targets a real Android device over `adb`.

> **iOS is not supported in the devcontainer.** iOS native builds require macOS/Xcode; build and
> run the iOS app on a Mac instead.

### How the phone reaches the container

The container runs its own `adb` server. Because the adb server lives in the container,
`adb reverse` points the phone's `localhost:8081` at the container's Metro — no LAN exposure or
Expo tunnel needed.

#### Wired (default, via usbipd-win)

The phone's USB is passed from Windows into WSL2 and on into the container through the
`/dev/bus/usb` mount declared in `.devcontainer/devcontainer.json` (the container already runs
`--privileged`). This is the primary, recommended workflow.

**One-time setup on Windows** (install [usbipd-win](https://github.com/dorssel/usbipd-win), then
in an **admin** PowerShell):

```powershell
usbipd list                          # find the phone's BUSID
usbipd bind   --busid <BUSID>        # one-time per device
```

**Each session, on Windows** (attach the phone into WSL2 *before* opening/rebuilding the
container so `/dev/bus/usb` exists):

```powershell
usbipd attach --wsl --busid <BUSID>
```

> The `/dev/bus/usb` bind mount requires that path to exist in WSL2 when the container starts.
> If no device has been attached, the directory may be absent and the container will fail to
> start — attach the phone first. Detach later with `usbipd detach --busid <BUSID>`.

**Each session, inside the container:**

```bash
# reverse-forward Metro for the already-attached USB device
app/scripts/adb-connect.sh

# then build/install the dev-client and start Metro
cd app
npx expo run:android      # first run, or after native/plugin/permission changes
# subsequent JS-only sessions:
npx expo start --dev-client
```

Editing `.ts`/`.tsx` triggers fast refresh on the phone over the adb channel.

#### Wireless (fallback, via TCP/IP)

If USB passthrough isn't viable, use wireless debugging instead. One-time per phone (on the host,
phone on USB, Developer Options enabled):

```bash
adb tcpip 5555
```

Then unplug, find the phone IP under *Settings > About phone > Status > IP address*, and inside
the container run:

```bash
app/scripts/adb-connect.sh <phone-ip>
```

Make sure no other `adb` server (e.g. on the host) is holding the device.

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
