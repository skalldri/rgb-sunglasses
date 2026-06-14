# USB forwarding into the dev container (CDC-ACM serial + mass storage + J-Link)

**Status:** Implemented & committed; **not yet verified end-to-end on hardware.**
**Date:** 2026-06-14
**Branch:** `react-native-devcontainer` (commits not pushed)
**User-facing runbook:** [`.devcontainer/USB.md`](../../.devcontainer/USB.md) — read that for "how to use".
This file is the engineering context / handoff, in case a container rebuild loses the chat.

---

## Problem

The custom board (`fw/`, an nRF5340/Zephyr device, USB ID `2fe3:0001`) is forwarded from
Windows into the dev container with `usbipd-win`. It enumerated (`lsusb` showed it), but its
**CDC-ACM serial ports** (`/dev/ttyACM*`) and **USB mass-storage** volume (`/dev/sd*`) never
appeared, so they couldn't be used.

## Root cause (verified, not the original hypothesis)

The drivers are present in the kernel but were never **loaded** — they are not missing.

- The container shares the **WSL2 kernel** `6.18.33.1-microsoft-standard-WSL2` (a container
  cannot have its own kernel). Confirmed: `uname -r`, cgroup `0::/`.
- `/proc/config.gz` shows the drivers are compiled as loadable modules:
  - `CONFIG_USB_ACM=m` (CDC-ACM serial)
  - `CONFIG_USB_STORAGE=m` (USB mass storage)
  - `CONFIG_MODULES=y`; `vhci_hcd`/`usbcore` are already loaded (usbipd loads them).
- `cdc_acm` and `usb_storage` were simply never `modprobe`'d, so nothing bound the device's
  serial/storage interfaces.
- `/lib/modules` **does not exist inside the container**, so you cannot `modprobe` from
  inside it.

## Environment decisions (from the user)

- Docker engine: **Docker Desktop (WSL2 backend)**.
- Project opened in **Windows VS Code** → "Reopen in Container".
- Accepted a **one-time** manual `usbipd bind` (Windows requires admin to share a USB
  device; the bind is persistent across reboots).

## Why the fix must touch the host (and how it stays hands-off)

A Docker Desktop container can't load modules itself: the kernel and `usbipd` live on the
host, and Docker Desktop's container host (the minimal `docker-desktop` distro) ships no
`/lib/modules`, so the usual `-v /lib/modules:/lib/modules` + in-container `modprobe` finds
nothing. The module load and the usbipd attach must run on the host.

The dev-container spec's **`initializeCommand`** runs on the host automatically every time
the container is created/started, before it boots. We use it to attach the device and load
the modules into the shared WSL2 kernel. Because the container is `--privileged`, the
resulting `/dev` nodes (created in the shared devtmpfs) are then visible inside it.

All distros in the WSL2 VM share one kernel, so `modprobe` run in a regular WSL2 distro
(which ships the matching `.ko` files — it already loaded `vhci_hcd`) loads the driver
globally, including for the Docker Desktop container.

## What was implemented

| File | Change |
|------|--------|
| `.devcontainer/scripts/host-usb-init.ps1` | **New.** Run by `initializeCommand` on the Windows host. For each target device: checks it's bound, starts a hidden `usbipd attach --auto-attach` (re-attaches after replug), then `modprobe -a cdc-acm usb-storage vhci-hcd usbip-host` via a real (non-`docker-desktop`) WSL2 distro. Best-effort: a script-scope `trap` + early `exit 0` paths ensure it never blocks the container. |
| `.devcontainer/scripts/usbip-bind.ps1` | **New.** One-time elevated helper: `usbipd bind` for each device. Checks for admin + usbipd. |
| `.devcontainer/devcontainer.json` | **Edited.** Added `initializeCommand` (with `\|\| exit 0` so non-Windows hosts without `powershell` don't fail container creation) and a diagnostic `postStartCommand`. Kept `--privileged` and the existing `postCreateCommand`. |
| `.devcontainer/USB.md` | **New.** User-facing runbook. |
| `README.md` | **Edited.** Added a USB bullet linking to `USB.md`. |

**Target devices** (default `$HardwareIds` in both scripts):
`2fe3:0001` (board) and `1366:0101` (SEGGER J-Link debug probe). Each is only
attached/bound when actually connected **and** bound; missing/unbound devices are skipped
with a warning. The J-Link is forwarded so the in-container nRF Connect / J-Link tooling can
flash & debug (it won't be visible to Windows-side tools while attached).

## Commits (on `react-native-devcontainer`, not pushed)

- `f4052a2` "Automating usb setup" — base implementation (committed by the user mid-work,
  so it captured the *pre-hardening* versions).
- `4620006` "Forward J-Link too; make USB init non-blocking" — adds J-Link to defaults;
  `|| exit 0`; script-scope `trap`.

## Open items / next steps

1. **Verify end-to-end on hardware** — none of this has been run against a live
   `usbipd`/`wsl` yet, and the PowerShell scripts could **not** be linted in-container
   (no `pwsh` there). See the verification steps below.
2. **Push / open a PR** when ready (`react-native-devcontainer` → `main`). Not done yet.
3. If `modprobe` reports a missing `.ko` in the chosen WSL distro, build it against the WSL2
   kernel source — see <https://github.com/rohzb/wsl2-usb-devices>. (Unlikely;
   cdc-acm/usb-storage ship with the stock kernel.)

## Verify end-to-end

1. **Once**, elevated Windows PowerShell: `.\.devcontainer\scripts\usbip-bind.ps1`
   → `usbipd list` shows the device(s) `Shared`.
2. Windows VS Code → **Reopen in Container** (`initializeCommand` runs automatically). Check
   the *Dev Containers* log for `[usb-init]` lines.
3. Inside the container:
   ```bash
   lsusb | grep -i 2fe3        # NordicSemiconductor RGB Sunglasses
   ls -l /dev/ttyACM*          # CDC-ACM serial ports
   lsblk ; ls /dev/sd*         # the mass-storage disk
   dmesg | tail                # cdc_acm + usb-storage binding
   sudo mkdir -p /mnt/dev && sudo mount /dev/sdX1 /mnt/dev   # read the MSC volume
   ```
4. Replug the board → auto-attach re-binds it without reopening the container.
5. `wsl --shutdown` then reopen → `initializeCommand` re-establishes everything.

## Key gotchas to remember

- **`initializeCommand` failing aborts container creation** — hence `|| exit 0` and the
  `trap`. Keep all paths exit-0.
- The WSL distro used for `modprobe` must be a real one (not `docker-desktop`); the script
  picks the first non-`docker-desktop` distro from `wsl -l -q`.
- `usbipd bind` genuinely cannot be automated (Windows admin requirement); it's persistent,
  so it's truly one-time.
- `devcontainer.json` is JSONC (comments allowed); validate by stripping `//` lines before
  `JSON.parse` if scripting a check.
