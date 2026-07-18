#!/usr/bin/env bash
# Flashes the RGB Sunglasses firmware over the attached J-Link, auto-detecting
# its serial number so callers never have to hardcode --dev-id.
#
# Usage: jlink-flash.sh [build-dir] [-- extra west flash args]
set -euo pipefail

JLINK_VID_PID="1366:0101"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/fw/build"

# This script is Linux/devcontainer-only (lsusb probe, nrfutil runner, SEGGER
# tools). On a macOS host, flash over MCUmgr serial OTA instead.
if [ "$(uname -s)" = "Darwin" ]; then
    echo "[!] jlink-flash.sh does not support macOS — use fw/scripts/mcumgr-flash.sh (MCUmgr OTA over serial)." >&2
    echo "    See fw/CLAUDE.md, 'macOS host (Mac Mini)'." >&2
    exit 1
fi

# The 'board' hw-lock coordinates the shared dev board across Claude Code agent
# worktrees. Only enforce it when an agent is driving — Claude Code sets CLAUDECODE=1
# in every command it spawns; a solo human developer flashes lock-free. Set
# RGBSG_NO_LOCK=1 to force the lock-free path even under an agent.
# See scripts/hw-lock.sh, .claude/skills/hw-lock/SKILL.md.
if [ -n "${CLAUDECODE:-}" ] && [ -z "${RGBSG_NO_LOCK:-}" ]; then
    if ! "$REPO_ROOT/scripts/hw-lock.sh" check board; then
        echo "[!] Refusing to flash: the 'board' hardware lock is not held by this session." >&2
        echo "    Run: Monitor(command: \"scripts/hw-lock.sh hold board\", persistent: true)   (see the hw-lock skill)" >&2
        exit 1
    fi
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
