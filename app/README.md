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
npm run ios   # macOS only — see "iOS (macOS)" below; not supported in the devcontainer
```

Note: BLE features require a native/dev build. Do not assume Expo Go supports the full BLE workflow.

## Developing in the Devcontainer (Android)

The repo's devcontainer (`.devcontainer/`) ships the firmware toolchain **and** a React Native
Android toolchain (Node, JDK 17, Android SDK/NDK), so you can build and run the Android app
without leaving the container. BLE only works on a **physical phone** (not an emulator or Expo
Go), so the dev loop targets a real Android device over `adb`.

> **iOS is not supported in the devcontainer.** iOS native builds require macOS/Xcode; build and
> run the iOS app on a Mac instead — see [iOS (macOS)](#ios-macos).

### How the phone reaches the container

The container runs its own `adb` server. `adb reverse` points the phone's `localhost:8081` at the
container's Metro — no LAN exposure or Expo tunnel needed. The recommended connection method is
wireless ADB; the phone and container only need to be on the same network.

#### Wireless (recommended)

No USB passthrough or usbipd required. Two sub-approaches depending on Android version.

**Android 11+ — no USB needed at all:**

1. On the phone: _Settings > Developer Options > Wireless Debugging_ — toggle it on.
2. Tap _Pair device with pairing code_; note the **IP address**, **pair port**, and **pairing
   code** shown on screen.
3. Inside the container, pair once:
   ```bash
   adb pair <phone-ip>:<pair-port>
   # enter the 6-digit pairing code when prompted
   ```
4. Back on the phone, the _Wireless Debugging_ screen shows a separate **IP address & Port** for
   the debug connection (different from the pair port). Use that port to connect:
   ```bash
   app/scripts/adb-connect.sh <phone-ip> <debug-port>
   ```

**Older Android (Android 10 and below) — USB required once per boot to enable TCP/IP mode:**

1. Connect the phone via USB to the host machine with USB debugging enabled.
2. On the host (not inside the container): `adb tcpip 5555`
3. Unplug USB. Find the phone IP under _Settings > About phone > Status > IP address_.
4. Inside the container:
   ```bash
   app/scripts/adb-connect.sh <phone-ip>
   ```

**After connecting (either approach), build and run:**

```bash
cd app
npx expo run:android --device <device name> --app-id com.autom8ed.rgbsunglassesapp.dev      # first run, or after native/plugin/permission changes
```

Editing `.ts`/`.tsx` triggers fast refresh on the phone over the adb tunnel.

#### Wired (fallback, via usbipd-win)

USB passthrough from Windows → WSL2 → container using
[usbipd-win](https://github.com/dorssel/usbipd-win). This is more fragile than wireless and
requires the phone to be attached before the container starts.

**One-time setup on Windows** (admin PowerShell):

```powershell
usbipd list                          # find the phone's BUSID
usbipd bind   --busid <BUSID>        # one-time per device
```

**Each session, on Windows** (attach _before_ opening or rebuilding the container):

```powershell
usbipd attach --wsl --busid <BUSID>
```

**Each session, inside the container:**

```bash
app/scripts/adb-connect.sh

cd app
npx expo run:android --device <device name> --app-id com.autom8ed.rgbsunglassesapp.dev
```

## iOS (macOS)

iOS native builds require macOS + Xcode, so they are built on a Mac (e.g. a Mac Mini M1) rather
than in the Linux devcontainer. The app is a managed Expo project — the `ios/` directory is
generated by `expo prebuild` and is not checked in.

### One-time setup

Run the self-setup script from the `app/` directory. It is idempotent and verifies/installs the
host toolchain (Xcode command line tools, Homebrew packages — node, watchman, CocoaPods), installs
the JS dependencies, and generates the native iOS project:

```bash
cd app
./scripts/macos-setup.sh
```

### Build and run

```bash
cd app
npm run ios            # build + launch on the iOS Simulator (or: npx expo run:ios)
```

To target a specific simulator, pass it explicitly, e.g. `npx expo run:ios --device "iPhone 17"`.

### BLE and the simulator

> **The iOS Simulator has no Bluetooth radio.** Device scanning will find nothing in the
> simulator, so the simulator is only useful for verifying the build, UI, and navigation.
> Exercising the live BLE workflow (scan → connect → control → firmware update) requires a
> **physical iPhone**.

The minimum deployment target is the Expo SDK 54 default (iOS 15.1), which covers every iPhone
Apple currently supports.

### CI

`.github/workflows/app-ci.yml` builds the iOS app for the simulator (unsigned) on a self-hosted
macOS (Apple Silicon) runner, mirroring the Android debug build. No Apple Developer account or
signing credentials are required for the simulator build.

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
