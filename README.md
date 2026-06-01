# RGB Sunglasses

[![codecov](https://codecov.io/gh/skalldri/rgb-sunglasses/graph/badge.svg)](https://codecov.io/gh/skalldri/rgb-sunglasses)

Monorepo for the RGB Sunglasses project.

## Layout

| Directory | Contents |
|-----------|----------|
| [`fw/`](fw/) | Firmware (Zephyr / Nordic Connect SDK) for the nRF5340-based sunglasses. See [`fw/README.md`](fw/README.md). |
| `app/` | React Native control app (imported separately). |
| `emulator/` | Firmware/display emulator (future). |

## Development

The firmware builds inside the dev container defined in
[`.devcontainer/`](.devcontainer/). Open the repo in VS Code and reopen in the
container; the nRF Connect extension is configured to find the firmware
application under `fw/`. Build and test commands live in
[`fw/CLAUDE.md`](fw/CLAUDE.md).
