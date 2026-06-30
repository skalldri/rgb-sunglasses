---
name: check-hardware
description: Check which development hardware is available (dev board, J-Link, Android device)
allowed-tools: Bash(.devcontainer/scripts/check-hardware.sh)
---

Run `.devcontainer/scripts/check-hardware.sh` and show the output verbatim.

If Android shows NOT CONNECTED and the user wants to pair a device, guide them through `adb pair` then `adb connect` — do not suggest QR code pairing (mDNS fails inside the container).
