# RGB Sunglasses — Firmware

[![codecov](https://codecov.io/gh/skalldri/rgb-sunglasses/graph/badge.svg)](https://codecov.io/gh/skalldri/rgb-sunglasses)

Zephyr RTOS / Nordic Connect SDK firmware for the **nRF5340**-powered RGB
Sunglasses — programmable LED sunglasses you control from your phone over
Bluetooth Low Energy.

🌐 **Project site & documentation:** <https://rgb-sunglasses.autom8ed.com/>

[![Open in Dev Containers](https://img.shields.io/static/v1?label=Dev%20Containers&message=Open&color=blue&logo=visualstudiocode)](https://vscode.dev/redirect?url=vscode%3A%2F%2Fms-vscode-remote.remote-containers%2FcloneInVolume%3Furl%3Dhttps%3A%2F%2Fgithub.com%2Fskalldri%2Frgb-sunglasses)

## What it does

- **Addressable LED display** — drives a dual-bank WS2812 LED panel as one logical
  40×12 display, plus the Compute Pack's on-board status LEDs.
- **Animations** — built-in effects (ZigZag, Rainbow, Text, My Eyes, Beat, FFT Bars,
  Matrix Code, Plasma, Tilt, …), a **GLIM player** for stored animation clips, and
  **sandboxed loadable `.llext` extensions** that appear as first-class animations.
- **Bluetooth control** — a compile-time-assembled GATT server exposes animations,
  per-animation parameters, and device settings; every setting persists across
  reboots. Requires LE Secure Connections pairing (`BT_SECURITY_L4`).
- **Over-the-air updates** — firmware images are published as GitHub Releases and
  installed from the companion app over BLE (MCUmgr/SMP), or over USB for developers.
- **Power** — on-board battery charging (BQ25792) with USB Power Delivery (TPS25750);
  charge state and battery level surface over BLE and on the status LEDs.
- **Sensors** — audio-reactive animations from a PDM microphone (VM3011) via a
  CMSIS-DSP FFT pipeline, and motion from a BMI270 IMU.
- **Storage & diagnostics** — a FAT volume on external QSPI flash, exposed over USB
  mass storage, holds animation assets; fatal faults capture a coredump for
  post-mortem debugging.

## Architecture

This is a Zephyr **sysbuild** project spanning both cores of the nRF5340:

| Image           | Core            | Role                                   |
| --------------- | --------------- | -------------------------------------- |
| `MCUboot`       | App core        | Secure bootloader                      |
| `rgb-sunglasses`| App core        | Main application (this firmware)        |
| `b0n`           | Network core    | Network-core bootloader                |
| `ipc_radio`     | Network core    | BLE controller (radio) firmware        |

The current hardware target is the **`rgb_sunglasses_proto0`** board. (The legacy
`rgb_sunglasses_dk` board was retired from `main` and lives on the frozen
`dk-support` branch.)

## Build & flash (no J-Link needed)

Everything builds inside the repo's dev container. Build and flash over a plain
USB-C cable with the helper scripts:

```bash
fw/scripts/build-fw.sh        # build the proto0 firmware (app + net core + MCUboot)
fw/scripts/mcumgr-flash.sh    # upload over USB (MCUmgr) and reboot into it
```

Have a SEGGER J-Link? `fw/scripts/jlink-flash.sh` is the faster path (and the only
way to reflash the bootloaders).

- **Full setup walkthrough** (Windows → dev container → build → flash → app) →
  [Developer Setup](https://rgb-sunglasses.autom8ed.com/developer-setup)
- **Recover a bricked board** (MCUboot DFU mode, no J-Link) →
  [Firmware Recovery](https://rgb-sunglasses.autom8ed.com/recovery) (in-repo:
  [`docs/flashing-without-jlink.md`](docs/flashing-without-jlink.md))

## Test

Firmware tests run on Zephyr's `native_sim` (no hardware required):

```bash
twister -T fw/tests -p native_sim
```

## Documentation

Full guides live on the [project site](https://rgb-sunglasses.autom8ed.com/docs):

- [Proto0 User Guide](https://rgb-sunglasses.autom8ed.com/proto0-user-guide) — set up
  and use the hardware.
- [Proto0 Assembly Guide](https://rgb-sunglasses.autom8ed.com/proto0-assembly-guide) —
  assemble the glasses.
- [Developer Setup](https://rgb-sunglasses.autom8ed.com/developer-setup) — build &
  flash from a fresh machine.
- [Firmware Recovery](https://rgb-sunglasses.autom8ed.com/recovery) — un-brick a board.
- [Proto0 Hardware Hacker's Guide](https://rgb-sunglasses.autom8ed.com/proto0-hardware-guide)
  — JTAG, jumpers, and connectors.

Deep build/test reference and subsystem internals live in
[`fw/CLAUDE.md`](CLAUDE.md).
