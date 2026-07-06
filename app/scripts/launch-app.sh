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
# The relationship is asymmetric, not fully independent: stopping the `hold`
# task (e.g. via TaskStop, which triggers its own release trap), or running
# `scripts/hw-lock.sh release app --force` yourself, now also stops Metro if
# it's still running (this script records its own pid against the lock right
# before exec-ing into Metro, so release can find and stop it precisely) --
# releasing the lock reliably means Metro has quit too. But it doesn't run in
# reverse: Metro dying or being stopped on its own still does NOT release the
# lock -- you still manage that side yourself.
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
    # Record our own pid (exec below keeps it, so this becomes Metro's real
    # pid) so releasing the lock can stop Metro precisely -- see
    # cmd_note_metro_pid / cmd_release in scripts/hw-lock.sh. Refuse rather
    # than proceed without this: a failure here means the lock was released
    # in the gap between the check above and now, so launching would race
    # directly against the collision this lock exists to prevent.
    if ! "$HW_LOCK" note-metro-pid app "$$"; then
        echo "[!] Refusing to launch: could not record this process as the 'app' lock's tracked Metro pid (the lock may have just been released) -- re-acquire the lock and retry." >&2
        exit 1
    fi
else
    echo "[!] scripts/hw-lock.sh not found -- launching without the shared app lock." >&2
fi

cd "$REPO_ROOT/app"
exec npx expo run:android --app-id com.autom8ed.rgbsunglassesapp.dev "$@"
