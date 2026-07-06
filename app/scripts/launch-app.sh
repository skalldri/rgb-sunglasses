#!/usr/bin/env bash
# Launches the companion app. Requires the 'app' hardware lock to already be
# held by this session -- this script only *verifies* that and refuses to
# run otherwise (same pattern as fw/scripts/jlink-flash.sh /
# fw/scripts/provision-device.sh). It never acquires or releases the lock
# itself. Acquire it first via the Monitor-based `hold` mechanism -- see
# scripts/hw-lock.sh / .claude/skills/hw-lock/SKILL.md:
#
#   Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
#   timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
#
# Metro's lifetime and the lock's lifetime are independent: stopping this
# script (or Metro dying on its own) does NOT release the lock. That only
# happens when you stop the `hold` task (e.g. via TaskStop, which triggers
# its own release trap) or run `scripts/hw-lock.sh release app --force`
# yourself.
#
# Run this itself as the harness-managed background task (same
# run_in_background: true convention as the raw npx command used to be) --
# do not background it with '&' by hand, and do not call npx expo
# run:android directly anymore.
#
# Usage: app/scripts/launch-app.sh [extra expo run:android args, e.g. --device <name>]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HW_LOCK="$REPO_ROOT/scripts/hw-lock.sh"

if [ -x "$HW_LOCK" ]; then
    if ! "$HW_LOCK" check app; then
        echo "[!] Refusing to launch: the 'app' hardware lock is not held by this session." >&2
        echo "    Run: Monitor(command: \"scripts/hw-lock.sh hold app\", persistent: true) first (see the hw-lock skill)." >&2
        exit 1
    fi
else
    echo "[!] scripts/hw-lock.sh not found -- launching without the shared app lock." >&2
fi

cd "$REPO_ROOT/app"
exec npx expo run:android --app-id com.autom8ed.rgbsunglassesapp.dev "$@"
