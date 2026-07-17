# RGB Sunglasses

[![codecov](https://codecov.io/gh/skalldri/rgb-sunglasses/graph/badge.svg)](https://codecov.io/gh/skalldri/rgb-sunglasses)

Monorepo for the RGB Sunglasses project.

## Layout

| Directory | Contents |
|-----------|----------|
| [`fw/`](fw/) | Firmware (Zephyr / Nordic Connect SDK) for the nRF5340-based sunglasses. See [`fw/README.md`](fw/README.md). |
| [`app/`](app/) | React Native (Expo) control app. See [`app/README.md`](app/README.md). |
| `emulator/` | Firmware/display emulator (future). |

## Development

Both the firmware and the Android app build inside the dev container defined in
[`.devcontainer/`](.devcontainer/). Open the repo in VS Code and reopen in the
container.

- **Firmware:** new to the project? The
  [**Developer Setup**](https://rgb-sunglasses.autom8ed.com/developer-setup) guide walks
  through the whole Windows → devcontainer workflow (build, flash over USB — no J-Link
  needed, deploy the app). Deeper build/test reference lives in
  [`fw/CLAUDE.md`](fw/CLAUDE.md); recovering a bricked board is covered in
  [Firmware Recovery](https://rgb-sunglasses.autom8ed.com/recovery).
- **App (Android):** the container also includes the React Native toolchain
  (Node, JDK 17, Android SDK/NDK). Connect a physical Android phone via wireless
  ADB (phone and container on the same network) and run the Expo dev-client — see
  [`app/README.md`](app/README.md#developing-in-the-devcontainer-android).
- **App (iOS):** iOS native builds require macOS/Xcode and so are built on a Mac
  (e.g. a Mac Mini M1) rather than in the container. Run the self-setup script
  [`app/scripts/macos-setup.sh`](app/scripts/macos-setup.sh) once, then
  `npm run ios` — see [`app/README.md`](app/README.md#ios-macos). iOS is also
  built in CI on a self-hosted macOS runner — triggered on push and manual
  `workflow_dispatch`, never on pull requests (see
  `.github/workflows/app-ios-ci.yml`).
- **USB device (serial + mass storage):** the board's CDC-ACM serial ports and
  mass-storage volume are forwarded into the container from Windows via `usbipd`.
  This is automatic after a one-time `usbipd bind` — see
  [`.devcontainer/USB.md`](.devcontainer/USB.md).
