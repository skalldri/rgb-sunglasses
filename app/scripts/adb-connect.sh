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
#   Wireless (recommended — phone and container on the same network):
#
#     Android 11+: enable Wireless Debugging in Developer Options.
#       Pair once:  adb pair <phone-ip>:<pair-port>   (enter pairing code)
#       Then:       ./scripts/adb-connect.sh <phone-ip> <debug-port>
#
#     Older Android: with phone on USB on the host run `adb tcpip 5555`, then:
#       ./scripts/adb-connect.sh <phone-ip>
#
#   Wired (fallback, via usbipd-win):
#     On Windows, attach the phone's USB into WSL2 first:
#       usbipd list
#       usbipd bind   --busid <BUSID>      # one-time, admin shell
#       usbipd attach --wsl --busid <BUSID>
#     Then run without arguments:
#       ./scripts/adb-connect.sh
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
# Tag each device line (USB)/(WiFi): a USB serial has no ":port" suffix, a
# TCP/WiFi serial is "ip:port" (issue #202 follow-up — report connection kind,
# not just presence).
adb devices | tail -n +2 | while IFS= read -r line; do
    [ -n "$line" ] || continue
    serial=$(echo "$line" | awk '{print $1}')
    case "$serial" in
        *:*) echo "  $line (WiFi)" ;;
        *) echo "  $line (USB)" ;;
    esac
done

if ! adb get-state >/dev/null 2>&1; then
    echo "error: no device detected." >&2
    echo "  Wireless: pass the phone IP (and port for Android 11+) as arguments." >&2
    echo "  Wired:    attach the phone with 'usbipd attach --wsl' on Windows first." >&2
    exit 1
fi

echo "Reverse-forwarding Metro (device:${METRO_PORT} -> container:${METRO_PORT}) ..."
adb reverse "tcp:${METRO_PORT}" "tcp:${METRO_PORT}"

echo "Done. Start the dev server with: npx expo start --dev-client"
