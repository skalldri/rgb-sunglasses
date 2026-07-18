# Firmware Recovery — MCUboot DFU mode

If your glasses **won't boot the app** — a bad flash, an interrupted update, a boot
loop, or corrupted main firmware — the normal update paths are unavailable. This guide
recovers the board over **just a USB cable** using MCUboot's built-in serial **DFU**
(Direct Firmware Update) mode. No J-Link required.

> **Board still boots and you just want to flash a new build?** You don't need DFU —
> use the normal MCUmgr flashing in
> [Developer Setup → Flash the firmware](/developer-setup) (`fw/scripts/mcumgr-flash.sh`).
> Come here only when the board **won't boot**.

> **What this can and can't do.** DFU recovery restores the **application** firmware. It
> does **not** reflash MCUboot itself — that needs a J-Link (or the in-app "Bootloader
> Update" feature). As long as MCUboot is intact (the normal case), a bricked app is
> fully recoverable cable-only.

---

## Before you start

You need the `mcumgr` tool, a firmware image, and USB access to the board.

- **In the project dev container:** nothing to install — `mcumgr` is already on the
  `PATH` and a build lives in `fw/build/`. (See [Developer Setup](/developer-setup) to
  get set up.)
- **On your own computer:** install `mcumgr` (needs [Go](https://go.dev/dl/)) and make
  sure you can reach the serial port:
  ```bash
  go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
  export PATH="$PATH:$(go env GOPATH)/bin"   # if ~/go/bin isn't already on PATH
  ```
  Serial access — Linux: add yourself to the `dialout` group; macOS: works as
  `/dev/tty.usbmodem*`; Windows/WSL2: forward the device with `usbipd`, see
  [`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md).

For the firmware image, use a local build (`fw/build/` — see
[Developer Setup](/developer-setup)) or download a release's
`dfu_application_proto0.zip` and point the script at the unzipped folder with
`--build-dir`.

---

## 1. Put the board into DFU mode

1. Unplug the board (or be ready to press reset).
2. **Press and hold the Left button** — wired to **P1.11** (`sw1` / `button1` in the
   schematic), the left-hand push button; see
   [`proto0-board-pinout.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/docs/proto0-board-pinout.md)
   — and keep holding.
3. **While still holding**, plug in USB (or press reset). Keep holding for a full second
   or so, then release.

MCUboot only enters DFU if the button is held for ~500 ms at power-on, so a brief
accidental press during normal use won't trigger it.

## 2. Confirm you're in DFU mode

In DFU mode the board stops being the app device and re-enumerates as MCUboot's own USB
device — **`2fe3:0100`, product string `MCUBOOT`** (the app runtime is `2fe3:0001`).
Check with `lsusb | grep -i 2fe3`. If in doubt, just run the recovery command below — it
looks specifically for the DFU device and tells you if it can't find one. (In the dev
container this device only reaches you if it's forwarded — see
[`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md),
"MCUboot recovery (DFU) mode".)

## 3. Recover the app core

```bash
fw/scripts/mcumgr-flash.sh --recovery
```

This writes the **application** image directly into its bootable slot and resets the
board out of DFU mode. There's no test/confirm step in DFU mode — the app runs on the
next boot. When the board comes back up as `2fe3:0001`, the app core is recovered.

> **DFU restores the app core only.** The network-core (radio) image lives in a slot
> MCUboot can only update through its normal swap path, not a direct DFU write — so this
> step deliberately skips it. That's fine: the app core boots and brings up USB on its
> own (independent of the radio), which is all you need for step 4.

## 4. Update the network core (for a full recovery)

Once the app boots (step 3), finish up with a normal MCUmgr update — which the running
app applies through MCUboot's proper swap + PCD path, restoring the network core:

```bash
fw/scripts/mcumgr-flash.sh
```

(This is the same normal flashing from [Developer Setup](/developer-setup).) After it
reboots, the board is fully recovered. If only the app was damaged, step 3 alone is
enough.

---

## Verifying it worked

- The glasses boot normally (LEDs / animations return).
- Reconnect with the companion app, or check over the serial shell (`mcuboot_version`,
  `kernel uptime`) if you have shell access.

---

## Troubleshooting

**"could not find a board in MCUboot serial-recovery mode"**
The board may not actually be in DFU mode — repeat the button-hold sequence (step 1) and
confirm `2fe3:0100` shows up (`lsusb | grep -i 2fe3`). In the dev container, make sure
the DFU device is forwarded (see
[`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md)).

**The upload is slow**
Normal — it's a ~660 KB image over a serial link; the script shows a progress bar as it
goes. `--app-only` skips the network-core image.

**"the 'mcumgr' CLI was not found"**
Install it (see "Before you start"), or run inside the dev container where it's
preinstalled.

**The board doesn't re-enumerate after reset**
Give it a moment — a multi-image update copies both images on the first boot before the
USB ports come back. On Windows/WSL2, a re-enumeration can also drop the `usbipd`
forward — re-attach it (see
[`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md)).

---

## Board differences (proto0 vs DK)

- **proto0** (production hardware): supports button-hold DFU recovery as described above.
- **DK** (legacy dev board): has **no** button entry into DFU mode — recover it with a
  J-Link instead. DK firmware and tooling are maintained on the
  [`dk-support` branch](https://github.com/skalldri/rgb-sunglasses/tree/dk-support);
  `main`'s `mcumgr-flash.sh` no longer special-cases the DK, and firmware releases
  no longer include a DK image (the newest release with a `dfu_application_dk.zip`
  asset is the last one usable for DK OTA updates).
