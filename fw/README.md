# RGB Sunglasses

[![codecov](https://codecov.io/gh/skalldri/rgb-sunglasses/graph/badge.svg)](https://codecov.io/gh/skalldri/rgb-sunglasses)

Based on the `peripheral_status` example from the Nordic Connect SDK.

[Clone in VSCode](vscode://ms-vscode-remote.remote-containers/cloneInVolume?url=https%3A%2F%2Fgithub.com%2Fskalldri%2Frgb-sunglasses)

## Updating / recovering firmware without a J-Link

No SEGGER J-Link? You can update or recover firmware over just a USB cable using
MCUboot's serial recovery (DFU) mode and the `mcumgr` tool — see
[**Flashing & recovering firmware without a J-Link**](docs/flashing-without-jlink.md)
(also published at <https://rgb-sunglasses.autom8ed.com/recovery>). The helper script
is `fw/scripts/mcumgr-flash.sh`.
