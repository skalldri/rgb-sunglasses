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

- **Firmware:** the nRF Connect extension is configured to find the firmware
  application under `fw/`. Build and test commands live in
  [`fw/CLAUDE.md`](fw/CLAUDE.md).
- **App (Android):** the container also includes the React Native toolchain
  (Node, JDK 17, Android SDK/NDK). Connect a physical Android phone via wireless
  ADB (phone and container on the same network) and run the Expo dev-client — see
  [`app/README.md`](app/README.md#developing-in-the-devcontainer-android).
  iOS native builds require macOS/Xcode and are not supported in the container.
- **USB device (serial + mass storage):** the board's CDC-ACM serial ports and
  mass-storage volume are forwarded into the container from Windows via `usbipd`.
  This is automatic after a one-time `usbipd bind` — see
  [`.devcontainer/USB.md`](.devcontainer/USB.md).
