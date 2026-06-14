#!/usr/bin/env bash
#
# adb-connect.sh — make a physical Android phone reachable from the devcontainer
# and reverse-forward the Metro port so the phone loads the JS bundle from Metro
# running inside the container.
#
# Because the adb server lives in the container, `adb reverse` points the phone's
# localhost:8081 at the container's Metro — no LAN exposure or Expo tunnel needed.
#
# Two modes:
#
#   Wired (default, via usbipd-win):
#     On Windows, attach the phone's USB into WSL2 first:
#       usbipd list
#       usbipd bind   --busid <BUSID>      # one-time, admin shell
#       usbipd attach --wsl --busid <BUSID>
#     The container sees it through the /dev/bus/usb mount. Then just run:
#       ./scripts/adb-connect.sh
#
#   Wireless (fallback, via TCP/IP):
#     One-time per phone (on the host, phone on USB):  adb tcpip 5555
#     Find the phone IP in Settings > About phone > Status > IP address, then:
#       ./scripts/adb-connect.sh <phone-ip> [adb-port]
#
# Usage:
#     ./scripts/adb-connect.sh [phone-ip] [adb-port=5555] [metro-port=8081]
#
set -euo pipefail

PHONE_IP="${1:-}"
ADB_PORT="${2:-5555}"
METRO_PORT="${3:-8081}"

if ! command -v adb >/dev/null 2>&1; then
    echo "error: adb not found on PATH (is the Android SDK installed in this container?)" >&2
    exit 1
fi

if [[ -n "${PHONE_IP}" ]]; then
    echo "Wireless mode: connecting to ${PHONE_IP}:${ADB_PORT} ..."
    adb connect "${PHONE_IP}:${ADB_PORT}"
else
    echo "Wired mode: looking for a USB-attached device (via usbipd /dev/bus/usb) ..."
fi

echo "Connected devices:"
adb devices

if ! adb get-state >/dev/null 2>&1; then
    echo "error: no device detected. For wired mode, attach the phone with 'usbipd attach --wsl'" >&2
    echo "       on Windows first; for wireless, pass the phone IP as the first argument." >&2
    exit 1
fi

echo "Reverse-forwarding Metro (device:${METRO_PORT} -> container:${METRO_PORT}) ..."
adb reverse "tcp:${METRO_PORT}" "tcp:${METRO_PORT}"

echo "Done. Start the dev server with: npx expo start --dev-client"
