#!/usr/bin/env bash
# Flashes the RGB Sunglasses firmware over the attached J-Link, auto-detecting
# its serial number so callers never have to hardcode --dev-id.
#
# Usage: jlink-flash.sh [build-dir] [-- extra west flash args]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/fw/build"

# Shared J-Link serial-number probe (also used by run-device-tests.sh).
. "$REPO_ROOT/fw/scripts/lib/jlink.sh"

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

SERIAL=$(jlink_find_serial) || exit 1

echo "[*] Flashing via J-Link S/N $SERIAL, build dir: $BUILD_DIR"
exec west flash -d "$BUILD_DIR" --dev-id "$SERIAL" "$@"
