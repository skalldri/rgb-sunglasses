#!/usr/bin/env bash
# Launches the companion app, holding the shared 'app' hardware lock for the
# entire lifetime of the Metro process so only one Metro instance (and one
# agent driving the one physical phone) ever runs at a time. Run this itself
# as the harness-managed background task (same run_in_background: true
# convention as the raw npx command used to be) -- do not background it with
# '&' by hand, and do not call npx expo run:android directly anymore. See
# scripts/hw-lock.sh / .claude/skills/hw-lock/SKILL.md.
#
# Usage: app/scripts/launch-app.sh [--wait SECONDS] [extra expo run:android args, e.g. --device <name>]
#
# --wait SECONDS: instead of failing immediately when 'app' is held by
# *another* session, block (polling) for up to SECONDS waiting for it to free
# up before giving up. Not forwarded to npx. Omit for the default fail-fast
# behavior.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HW_LOCK="$REPO_ROOT/scripts/hw-lock.sh"

WAIT_ARGS=()
PASSTHROUGH_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --wait) WAIT_ARGS=(--wait "${2:-}"); shift 2 ;;
        *) PASSTHROUGH_ARGS+=("$1"); shift ;;
    esac
done
set -- "${PASSTHROUGH_ARGS[@]}"

if [ -x "$HW_LOCK" ]; then
    if ! "$HW_LOCK" acquire app --pid "$$" --fresh-only "${WAIT_ARGS[@]}" \
            --reason "expo run:android (launch-app.sh)"; then
        echo "[!] Refusing to launch: the 'app' lock is already held -- a Metro" >&2
        echo "    instance may already be running (possibly one you started earlier" >&2
        echo "    in this same task and forgot about). Do not start a second instance;" >&2
        echo "    stop the existing background task first, which releases this lock" >&2
        echo "    automatically (or pass --wait SECONDS to block until it does), then" >&2
        echo "    relaunch." >&2
        exit 1
    fi
else
    echo "[!] scripts/hw-lock.sh not found -- launching without the shared app lock." >&2
fi

CHILD_PID=""
cleanup() {
    local status=$?
    if [ -n "$CHILD_PID" ] && kill -0 "$CHILD_PID" 2>/dev/null; then
        kill -TERM "$CHILD_PID" 2>/dev/null || true
        wait "$CHILD_PID" 2>/dev/null || true
    fi
    if [ -x "$HW_LOCK" ]; then
        "$HW_LOCK" release app >/dev/null 2>&1 || true
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

cd "$REPO_ROOT/app"
npx expo run:android --app-id com.autom8ed.rgbsunglassesapp.dev "$@" &
CHILD_PID=$!
wait "$CHILD_PID"
