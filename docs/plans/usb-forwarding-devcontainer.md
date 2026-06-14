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
- `modprobe` **does** work in the `docker-desktop` WSL2 distro (confirmed by user). That
  distro is the kernel host and ships the matching `.ko` files.

## Environment decisions (from the user)

- Docker engine: **Docker Desktop (WSL2 backend)**.
- Project opened in **Windows VS Code** → "Reopen in Container".
- Accepted a **one-time** manual `usbipd bind` (Windows requires admin to share a USB
  device; the bind is persistent across reboots).

## Where initializeCommand actually runs (critical finding from first test)

When using **"Clone Repository in Container Volume"** (the typical Docker Desktop + VS Code
flow), VS Code routes `initializeCommand` through the `docker-desktop` WSL2 distro, not the
Windows host. The startup log shows:

```
Start: Run: /bin/sh -c powershell ... host-usb-init.ps1 || exit 0
/bin/sh: powershell: not found
```

VS Code on Windows connects to docker-desktop via `wsl -d docker-desktop /bin/sh -c ...`,
and the devcontainer CLI is bootstrapped inside a minimal Docker container there. The
`initializeCommand` is run in that context — where `powershell` (the Linux package) doesn't
exist.

**This means**:
- `initializeCommand` must be a shell (`/bin/sh`) script, not a PowerShell script.
- `docker-desktop` **does ship `/lib/modules`** for the running kernel (confirmed: the user
  ran `modprobe cdc_acm` there manually and it worked). So `modprobe` can be called directly
  from `initializeCommand` without needing a separate "real" WSL2 distro.
- `usbipd` (a Windows process) can be reached via Windows interop (`usbipd.exe`) if Docker
  Desktop has interop enabled in docker-desktop — this is best-effort and not guaranteed.

The previous plan assumed docker-desktop had no `/lib/modules` and that `initializeCommand`
ran on Windows. Both assumptions were wrong.

## What was implemented

| File | Change |
|------|--------|
| `.devcontainer/scripts/host-usb-init.sh` | **New (replaces .ps1 as initializeCommand).** Shell script that runs in docker-desktop. Calls `modprobe -a cdc-acm usb-storage vhci-hcd usbip-host` directly, then attempts `usbipd.exe attach --wsl --auto-attach` via Windows interop (best-effort; skipped with a warning if interop is unavailable). Always exits 0. |
| `.devcontainer/scripts/host-usb-init.ps1` | **Kept for manual use.** Can be run from Windows PowerShell directly for initial `usbipd` bind+attach setup, but is no longer the `initializeCommand` entry point (it never ran there — see above). |
| `.devcontainer/scripts/usbip-bind.ps1` | **New.** One-time elevated helper: `usbipd bind` for each device. Checks for admin + usbipd. |
| `.devcontainer/devcontainer.json` | **Edited.** `initializeCommand` now calls `host-usb-init.sh` (shell script). Diagnostic `postStartCommand` unchanged. Kept `--privileged` and the existing `postCreateCommand`. |
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
- (pending) Switch `initializeCommand` to `host-usb-init.sh`; add `modprobe` fix.

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

- **`initializeCommand` runs in docker-desktop, not Windows**, when using "Clone in
  Container Volume". Use a shell script, not PowerShell.
- **docker-desktop has `/lib/modules`** — `modprobe` works there directly. The earlier
  assumption that it didn't was wrong.
- **`usbipd.exe` via Windows interop** may or may not be available in docker-desktop
  depending on Docker Desktop version and interop settings. Treat it as best-effort.
- **`initializeCommand` script must always exit 0** — failure aborts container creation.
  `host-usb-init.sh` is written to always exit 0.
- `usbipd bind` genuinely cannot be automated (Windows admin requirement); it's persistent,
  so it's truly one-time.
- `devcontainer.json` is JSONC (comments allowed); validate by stripping `//` lines before
  `JSON.parse` if scripting a check.
