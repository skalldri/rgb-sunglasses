#!/usr/bin/env bash
# Flashes the RGB Sunglasses firmware over the attached J-Link, auto-detecting
# its serial number so callers never have to hardcode --dev-id.
#
# Usage: jlink-flash.sh [build-dir] [-- extra west flash args]
set -euo pipefail

JLINK_VID_PID="1366:0101"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/fw/build"

# Refuse to flash unless this session holds the 'board' hardware lock --
# flashing resets the board and would race with any other agent using the
# J-Link/serial console. See scripts/hw-lock.sh, .claude/skills/hw-lock/SKILL.md.
if ! "$REPO_ROOT/scripts/hw-lock.sh" check board; then
    echo "[!] Refusing to flash: the 'board' hardware lock is not held by this session." >&2
    echo "    Run: scripts/hw-lock.sh acquire board   (see the hw-lock skill)" >&2
    exit 1
fi

# A first arg that isn't an option (doesn't start with "-") is the build dir;
# anything else is forwarded to `west flash` as-is.
if [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
    BUILD_DIR="$1"
    shift
fi

if ! lsusb 2>/dev/null | grep -qi "$JLINK_VID_PID"; then
    echo "[!] J-Link ($JLINK_VID_PID) not detected on USB. Run /check-hardware first." >&2
    exit 1
fi

# JLinkExe only opens the USB connection lazily, on the first command that
# needs it - ShowHWStatus forces the connect so the banner (incl. S/N) prints.
PROBE_FILE=$(mktemp /tmp/jlink_probe.XXXXXX.jlink)
printf 'ShowHWStatus\nExit\n' > "$PROBE_FILE"
JLINK_OUT=$(timeout 10 JLinkExe -CommandFile "$PROBE_FILE" 2>&1 || true)
rm -f "$PROBE_FILE"

if ! echo "$JLINK_OUT" | grep -q "Connecting to J-Link via USB...O.K."; then
    echo "[!] USB present but J-Link Commander did not connect. Output:" >&2
    echo "$JLINK_OUT" >&2
    exit 1
fi

SERIAL=$(echo "$JLINK_OUT" | grep -oE 'S/N: [0-9]+' | grep -oE '[0-9]+' | head -n 1 || true)
if [ -z "$SERIAL" ]; then
    echo "[!] Connected to J-Link but could not parse serial number from output:" >&2
    echo "$JLINK_OUT" >&2
    exit 1
fi

echo "[*] Flashing via J-Link S/N $SERIAL, build dir: $BUILD_DIR"
exec west flash -d "$BUILD_DIR" --dev-id "$SERIAL" "$@"
