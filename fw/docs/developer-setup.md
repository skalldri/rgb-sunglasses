# Developer Setup — Windows + Devcontainer

This guide takes you from a fresh Windows PC to **building and flashing the firmware
and building and deploying the Android app**, using the project's VS Code
**devcontainer** (which ships the whole firmware + React Native toolchain).

It covers the **Windows → devcontainer** route. macOS/Linux hosts use the same
devcontainer; only the USB-forwarding step (3) differs.

> **The flashing steps below assume no J-Link programmer** (the common case). If you
> do have a SEGGER J-Link, there's a faster path noted in step 5. Recovering a board
> that won't boot is covered separately in [Firmware Recovery](/recovery).

---

## What you'll need

- A Windows PC with virtualization enabled.
- The **Proto0 hardware** and a USB-C cable — see the [Proto0 User Guide](/proto0-user-guide).
- An **Android phone** (for the app; BLE needs a physical device, not an emulator).
- *(Optional)* a SEGGER J-Link for fast firmware flashing / debugging.

---

## 1. Install host prerequisites (one-time)

Install on Windows:

1. **Docker Desktop** with the **WSL 2** backend (enable WSL 2 integration in Docker Desktop settings).
2. **Visual Studio Code** + the **Dev Containers** extension (`ms-vscode-remote.remote-containers`).
3. **Git**.
4. **usbipd-win** (forwards USB devices into the container):
   ```powershell
   winget install usbipd
   ```

---

## 2. Clone the repo and open it in the devcontainer

Easiest — one click from the [firmware README](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/README.md)'s
**Open in Dev Containers** badge (clones into a container volume). Or manually:

```bash
git clone https://github.com/skalldri/rgb-sunglasses.git
```

Open the folder in VS Code → when prompted, **"Reopen in Container"** (or run
*Dev Containers: Reopen in Container* from the command palette).

The **first** container build downloads the toolchain image and runs the setup
(`npm install` for the app, J-Link tooling, etc.) — this takes a while. When it
finishes you have a shell with `west`, `mcumgr`, the Android SDK, and everything else
preinstalled.

---

## 3. Forward USB devices into the container

The board's serial ports (and, later, the J-Link) reach the container from Windows via
`usbipd`. Full details and troubleshooting are in
[`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md);
the short version:

**Once**, in an **elevated** PowerShell (with the board plugged in):

```powershell
.\.devcontainer\scripts\usbip-bind.ps1
```

This binds the board (`2fe3:0001`), its DFU-mode device (`2fe3:0100`), and a J-Link
(`1366:0101`) if present. Binding is persistent across reboots. After binding, the
container's startup automatically attaches and loads the needed WSL kernel modules, so
you normally only ever do this once.

**Verify inside the container:**

```bash
.devcontainer/scripts/check-hardware.sh
```

You should see the dev board detected with two serial ports (`ttyACM0` = shell,
`ttyACM1` = MCUmgr).

> The **phone** does *not* use usbipd — the app step (6) connects to it over
> **wireless ADB** instead.

---

## 4. Build the firmware

One command (defaults to the proto0 board → `fw/build`):

```bash
fw/scripts/build-fw.sh
```

The **first** build is a full from-scratch configure (it builds the network-core image,
MCUboot, and the app), so it takes noticeably longer than the incremental builds after
it. For the legacy DK board: `fw/scripts/build-fw.sh dk`.

---

## 5. Flash the firmware (no J-Link)

Your board already ships running firmware, so you can update it over USB with **MCUmgr**
— no J-Link needed. Plug the board in and run:

```bash
fw/scripts/mcumgr-flash.sh
```

This finds the MCUmgr serial port, uploads your freshly built app image (showing a
progress bar as it goes), and reboots the board into it. That's the whole loop:
**edit → `build-fw.sh` → `mcumgr-flash.sh`**.

- **Have a J-Link?** It's much faster: `fw/scripts/jlink-flash.sh`.
- **Board won't boot / bricked?** Use MCUboot's DFU recovery mode — see
  [Firmware Recovery](/recovery).
- **Only need the app core?** `fw/scripts/mcumgr-flash.sh --app-only` skips the
  network core.

Confirm it worked over the serial shell — connect with `scripts/fw-shell.sh` and run
`kernel version` / `kernel uptime`.

---

## 6. Build and deploy the Android app

BLE only works on a **physical Android phone**. The container talks to it over
**wireless ADB** (no USB passthrough needed).

**a. Install JS dependencies** (once, and after dependency changes):

```bash
cd app && npm install
```

**b. Connect the phone over wireless ADB** (Android 11+):

1. On the phone: *Settings → Developer Options → Wireless Debugging* → turn it on.
2. Tap *Pair device with pairing code*; note the **IP**, **pair port**, and **6-digit code**.
3. In the container:
   ```bash
   adb pair <phone-ip>:<pair-port>       # enter the 6-digit code
   ```
4. Back on the *Wireless Debugging* screen, read the **debug IP:port** (different from the
   pair port) and connect:
   ```bash
   adb connect <phone-ip>:<debug-port>
   ```

Confirm with `adb devices` (it should list your phone). Wireless pairing is remembered by
the phone; on later sessions just re-run `adb connect <phone-ip>:<debug-port>`.

**c. Build, install, and run** (run this as a long-lived command — it also starts Metro):

```bash
app/scripts/launch-app.sh --device "<phone model name>"
```

Use the phone's **model name** (e.g. `Pixel_9_Pro`), *not* the ADB `ip:port`. With exactly
one device connected you can omit `--device` and it'll auto-select. `launch-app.sh` handles
the `.dev` app-id detail and runs the native build; after it's up, editing `.ts`/`.tsx`
files fast-refreshes on the phone.

> Prefer the raw command? `cd app && npx expo run:android --device "<model name>" --app-id
> com.autom8ed.rgbsunglassesapp.dev` — the `--app-id` is required (this project installs a
> side-by-side `.dev` build). See
> [`app/README.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/app/README.md).

**d. Pair the phone to the glasses over Bluetooth.** Open the app, accept the permission
prompts, and tap **Connect** on the detected `RGB Sunglasses` device. When Android asks to
pair, the glasses' LED panel scrolls a **6-digit passkey** — enter it to complete pairing.
Full walkthrough: [Proto0 User Guide](/proto0-user-guide#bluetooth-pairing-first-time).

---

## You're set up

The day-to-day loops are:

- **Firmware:** edit → `fw/scripts/build-fw.sh` → `fw/scripts/mcumgr-flash.sh` → verify.
- **App:** edit `.ts`/`.tsx` → fast refresh (or re-run `launch-app.sh` after native/permission changes).

### Where to go next
- [Proto0 User Guide](/proto0-user-guide) — the hardware itself (connectors, buttons, safety).
- [Firmware Recovery](/recovery) — un-brick a board with no J-Link.
- [`fw/CLAUDE.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/CLAUDE.md) and
  [`app/CLAUDE.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/app/CLAUDE.md) —
  deep reference for the firmware and app internals.

### Notes
- **iOS** is not built in the devcontainer (it needs macOS/Xcode) — see
  [`app/README.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/app/README.md#ios-macos).
- USB troubleshooting (ports not appearing, re-attach after replug):
  [`.devcontainer/USB.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/.devcontainer/USB.md).
