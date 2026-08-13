#!/usr/bin/env bash
# Flashes the RGB Sunglasses firmware over the attached J-Link, auto-detecting
# its serial number so callers never have to hardcode --dev-id.
#
# Usage: jlink-flash.sh [--recover] [build-dir] [-- extra west flash args]
#
# --recover: unlock + mass-erase the chip before flashing (west flash --recover).
#   Use when flashing fails with a memory/protection error — the runner's own hint
#   for that failure is "the target must be recovered". Named to match west's flag;
#   NOT the same as mcumgr-flash.sh --recovery (MCUboot serial DFU, no J-Link).
#
#   REQUIRES THE BATTERY CONNECTED: without it, power is not stable enough for the
#   recover/re-flash to complete — observed 2026-08-13 as nrfutil dying with
#   "Unable to detect CTRL-AP at 2" on every attempt until the battery was attached
#   (the same run then recovered and flashed cleanly). If you hit that error, check
#   the battery before suspecting the debug port or the chip.
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

# Pull out --recover wherever it appears (but leave everything after a literal
# "--" alone — that's the caller's explicit west-flash passthrough).
RECOVER=0
PASS=()
while [ "$#" -gt 0 ]; do
    if [ "$1" = "--recover" ]; then
        RECOVER=1
    elif [ "$1" = "--" ]; then
        PASS+=("$@")
        break
    else
        PASS+=("$1")
    fi
    shift
done
set -- ${PASS[@]+"${PASS[@]}"}

# A first arg that isn't an option (doesn't start with "-") is the build dir;
# anything else is forwarded to `west flash` as-is.
if [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
    BUILD_DIR="$1"
    shift
fi

SERIAL=$(jlink_find_serial) || exit 1

if [ "$RECOVER" -eq 1 ]; then
    # Zephyr's nRF runner recovers the net core first, then the app core (recovering
    # either erases both — nrf_common.py), and soc.yml marks --recover run-once so a
    # multi-domain sysbuild flash recovers exactly once, before the first image.
    echo "[*] Recover requested: unlocking + mass-erasing internal flash on BOTH cores (net core first)."
    echo "    Bonds/settings/NAND assets survive (external flash). Expect ONE warm reboot on first"
    echo "    boot while the firmware rewrites UICR VREGHVOUT to 3.3 V."
    echo "    NOTE: the battery must be connected — VBUS-only power is not stable enough to"
    echo "    recover ('Unable to detect CTRL-AP' failures until it is attached)."
    set -- --recover "$@"
fi

echo "[*] Flashing via J-Link S/N $SERIAL, build dir: $BUILD_DIR"
exec west flash -d "$BUILD_DIR" --dev-id "$SERIAL" "$@"
