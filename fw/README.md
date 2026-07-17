# RGB Sunglasses

[![codecov](https://codecov.io/gh/skalldri/rgb-sunglasses/graph/badge.svg)](https://codecov.io/gh/skalldri/rgb-sunglasses)

Based on the `peripheral_status` example from the Nordic Connect SDK.

[![Open in Dev Containers](https://img.shields.io/static/v1?label=Dev%20Containers&message=Open&color=blue&logo=visualstudiocode)](https://vscode.dev/redirect?url=vscode%3A%2F%2Fms-vscode-remote.remote-containers%2FcloneInVolume%3Furl%3Dhttps%3A%2F%2Fgithub.com%2Fskalldri%2Frgb-sunglasses)

## Building & flashing firmware (no J-Link needed)

Build and flash over just a USB cable with the helper scripts (`fw/scripts/build-fw.sh`,
`fw/scripts/mcumgr-flash.sh`):

- **Full dev setup + the build/flash loop** →
  [Developer Setup](https://rgb-sunglasses.autom8ed.com/developer-setup)
- **Recover a bricked board** (MCUboot DFU mode) →
  [Firmware Recovery](https://rgb-sunglasses.autom8ed.com/recovery) (in-repo:
  [`docs/flashing-without-jlink.md`](docs/flashing-without-jlink.md))
