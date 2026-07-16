# Flashing & recovering firmware without a J-Link

This guide is for updating or recovering the RGB Sunglasses firmware using only a
**USB cable** — no J-Link/SEGGER programmer required. It uses the nRF5340's built-in
**MCUboot** bootloader and the firmware's **MCUmgr/SMP** update server over serial.

There are two paths:

| Path | Use when | Command |
| ---- | -------- | ------- |
| **A — Update** | The glasses still power on and run the app | `fw/scripts/mcumgr-flash.sh` |
| **B — Recover (DFU)** | The board is bricked / won't boot the app | `fw/scripts/mcumgr-flash.sh --recovery` |

> **What this can and can't do.** These paths update the **application** firmware
> (the LED app on the main core and the radio image on the network core). They do
> **not** reflash MCUboot itself — that still needs a J-Link, or the in-app
> "Bootloader Update" feature. If MCUboot is intact (the normal case), everything
> below works cable-only.

---

## Before you start (one-time setup)

You need two things: the `mcumgr` command-line tool, and a firmware build to flash.

### If you use the project dev container

Nothing to install — `mcumgr` is already on the `PATH`, and the firmware build
lives in `fw/build/`. Skip to **Get a firmware image** below.

### On your own computer (no dev container)

1. **Install `mcumgr`** (needs [Go](https://go.dev/dl/)):

   ```bash
   go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
   # add Go's bin dir to your PATH if it isn't already (usually ~/go/bin)
   export PATH="$PATH:$(go env GOPATH)/bin"
   ```

2. **Make sure your user can access the serial port.**
   - **Linux:** add yourself to the `dialout` group (`sudo usermod -aG dialout $USER`,
     then log out and back in), or `sudo` the command.
   - **macOS:** the port shows up as `/dev/tty.usbmodemXXXX` — no extra permissions needed.
   - **Windows (WSL2):** the board's USB device must be forwarded into WSL with
     `usbipd` — see [`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md).

### Get a firmware image

Use a build you produced locally (`fw/build/` after building the firmware — see
[`fw/CLAUDE.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/CLAUDE.md)), or download a release's `dfu_application_proto0.zip`
from the project's GitHub Releases page and point the script at the folder you
unzipped it into with `--build-dir`.

The script prefers `dfu_application.zip` (it contains **both** the main-core and
network-core images plus a manifest); if only the signed app image is present it
flashes just that.

---

## Path A — update while the glasses still boot

1. Plug the glasses into your computer with a USB cable.
2. Run:

   ```bash
   fw/scripts/mcumgr-flash.sh
   ```

That's it. The script finds the right serial port, uploads the new image(s) to the
spare firmware slot (this takes **3–4 minutes** for the main image — be patient),
tells the bootloader to boot the new image, resets, and confirms it once the board
comes back.

Useful options:

- `--app-only` — skip the network-core image (faster; fine for most app changes).
- `--build-dir <dir>` — flash a different build (e.g. an unzipped release folder,
  or `fw/build-dk` for the DK board).
- `--no-confirm` — upload and boot the new image but don't mark it permanent yet.
- `--port /dev/ttyACMx` — force a specific serial port if auto-detection picks wrong.

---

## Path B — recover a bricked board (MCUboot DFU mode)

If the glasses won't boot the app (bad flash, interrupted update), MCUboot has a
**serial recovery** mode that accepts a fresh image even when the app is dead.

### 1. Put the board into recovery mode

1. Unplug the board (or be ready to press reset).
2. **Press and hold the Left button** and keep holding it.
   - This is the button wired to **P1.11** (`sw1` / `button1` in the schematic) —
     the left-hand push button. See [`proto0-board-pinout.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/docs/proto0-board-pinout.md).
3. **While still holding**, plug in USB (or press the reset button). Keep holding
   for a full second or so, then release.

MCUboot only enters recovery if the button is held for ~500 ms at power-on, so a
brief accidental press during normal use won't trigger it.

### 2. Confirm you're in recovery mode

In recovery the board stops being the app device and re-enumerates as MCUboot's own
USB device — **`2fe3:0100`, product string `MCUBOOT`** (the app runtime is
`2fe3:0001`). Check with `lsusb | grep -i 2fe3`. If in doubt, run
`fw/scripts/mcumgr-flash.sh --recovery` — it looks specifically for the recovery
port and tells you if it can't find one. (In the dev container this device only
reaches you if it's forwarded — see
[`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md),
"MCUboot recovery (DFU) mode".)

### 3. Recover the app core

```bash
fw/scripts/mcumgr-flash.sh --recovery
```

This writes the **application** image directly into its bootable slot and resets the
board out of recovery. There's no test/confirm step in recovery mode — the app runs
on the next boot. When it comes back up as `2fe3:0001`, the app core is recovered.

> **Recovery restores the app core only.** The network-core (radio) image lives in a
> slot MCUboot can only update through its normal swap path, not a direct recovery
> write — so the recovery step deliberately skips it. That's fine: the app core boots
> and brings up USB on its own (independent of the radio), which gives you everything
> you need for step 4.

### 4. Update the network core (for a full recovery)

Once the app has booted (step 3), finish the recovery with a normal app-mode update,
which updates the network core through MCUboot's proper path:

```bash
fw/scripts/mcumgr-flash.sh
```

After this reboot the board is fully recovered (both cores). If only the app was
damaged, step 3 alone is enough and you can skip this.

---

## Verifying it worked

- The glasses should boot normally (LEDs / animations return).
- Reconnect with the companion app, or check the firmware version over the serial
  shell (`mcuboot_version`, `kernel uptime`) if you have shell access.

---

## Troubleshooting

**"could not find the board's MCUmgr port" / "…recovery mode"**
The port numbering shifts every time the board resets or re-enumerates. Wait a few
seconds after plugging in, then re-run. If it still can't find it, list your serial
ports and pass the right one with `--port`. In the dev container, `/check-hardware`
prints the current ports.

**"no response from …"**
- Path A: the app may not actually be running — try Path B (`--recovery`).
- Path B: the board may not be in recovery mode — repeat the button-hold sequence.

**The upload is very slow**
That's normal — the main image is ~660 KB and serial transfers run around
3–4 KB/s, so ~3–4 minutes. `--app-only` skips the network-core image if you don't
need it.

**"the 'mcumgr' CLI was not found"**
Install it (see setup above), or run inside the dev container where it's preinstalled.

**The board doesn't re-enumerate after reset**
Give it up to ~90 seconds — a multi-image (app + network-core) update copies both
images on the first boot before the USB ports come back. On Windows/WSL2, a
re-enumeration can also drop the `usbipd` forward — re-attach it (see
[`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md)).

---

## Board differences (proto0 vs DK)

- **proto0** (production hardware): supports both paths fully — app-mode update and
  button-hold serial recovery.
- **DK** (legacy dev board): has **no** button-entry into serial recovery, and its
  MCUmgr build omits the reset command. Use `--build-dir fw/build-dk` for app-mode
  updates and reset the board manually when prompted; for recovery, use a J-Link.
</content>
