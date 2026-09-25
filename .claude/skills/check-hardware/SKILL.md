---
name: check-hardware
description: Check which development hardware is available (dev board, J-Link, Android device)
allowed-tools: Bash(.devcontainer/scripts/check-hardware.sh)
---

Run `.devcontainer/scripts/check-hardware.sh` and show the output verbatim.

The script probes the dev board (lsusb on Linux, IORegistry on macOS) and its TTY ports,
the J-Link (status, VTref, serial number), and the phone (Android via ADB in the
devcontainer; iPhone via `devicectl` on macOS, where the board's ports are
`/dev/cu.usbmodem*`). It also applies USB device-node fixes, which is why it can find a
device a bare `adb devices` / `lsusb` misses
(`docs/agent-incidents.md#2026-07-19-bare-adb-devices-reported-no-phone`).

- **Never run it while an app install is streaming.** When no device is already in `device`
  state it restarts the adb server, which kills an in-flight `adb install`
  (`docs/agent-incidents.md#2026-07-25-check-hardware-killed-an-in-flight-app-install`).
  Mid-deploy, use `lsusb | grep 2fe3` + `fw/scripts/fix-usb-dev-nodes.sh` for board checks.
- **If Android shows NOT CONNECTED, report it and stop** — never connect, pair or re-dial a
  device yourself, and never walk the user through it as a way of getting it back (root
  `CLAUDE.md` "NEVER connect an ADB device yourself"). Useful diagnosis to hand back: a
  missing device usually means the wireless-debugging TCP connection dropped, not that the
  pairing was lost (pairing is remembered by the phone); wireless debugging rotates to a new
  random port whenever it restarts, and mDNS discovery (QR pairing) does not work from inside
  the container.
