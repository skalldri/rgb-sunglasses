#!/usr/bin/env bash
# Launches the companion app on a physical iPhone — the iOS sibling of
# launch-app.sh, and the ONLY supported way to run `expo run:ios` here.
#
# Exists because of a real incident (2026-08-13): a bare `npx expo run:ios
# --device` against a stale ios/ prebuild — one predating BOTH the
# plugins/withDevVariantIos.js dev-variant plugin AND the #320 app rename —
# built Debug WITHOUT the .dev bundle-id suffix and silently installed it over
# the production `com.autom8ed.rgbsunglassesapp` app on the shared iPhone,
# replacing the TestFlight install. `expo run:ios` only *generates* ios/ when
# it's missing, exactly like the Android case launch-app.sh exists for
# (PR #224's missing notification drawable).
#
# Two defenses, in order:
#   1. Always re-run prebuild (incremental, no --clean) before building.
#      Verified live against the incident's actual stale tree: the sync
#      injects the dev-variant build settings (PRODUCT_BUNDLE_IDENTIFIER
#      .dev suffix, APP_DISPLAY_NAME, AppIcon-Dev) into the existing project
#      — including one whose very .xcodeproj name predates the app rename
#      (prebuild syncs into the existing project rather than renaming it, so
#      a local tree may legitimately be RGBSunglasses.* while fresh CI trees
#      are RGBGlasses.* — glob, never hardcode, the project name here).
#   2. Assert the result before building: the Debug configuration MUST carry
#      the .dev bundle id, or we refuse to build at all — a wrong prebuild
#      then fails loudly here instead of shipping a mislabeled app that
#      overwrites the production install.
#
# Requires the 'app' hardware lock to already be held by this session when an
# agent is driving (same check-only pattern as launch-app.sh — this script
# never acquires or releases the lock itself):
#
#   Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
#   timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
#
# Run as a harness-managed background task (run_in_background: true) — it
# execs into Metro and blocks for the session, same as launch-app.sh. Pass the
# iPhone's TRADITIONAL hardware UDID from `xcrun xctrace list devices`
# (00008120-…), not the CoreDevice UUID from `devicectl list devices`.
#
# Usage: app/scripts/launch-app-ios.sh --device <UDID> [extra expo run:ios args]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HW_LOCK="$REPO_ROOT/scripts/hw-lock.sh"

# The 'app' hw-lock coordinates the single shared companion-app phone across
# Claude Code agent worktrees (on the Mac host that phone is the iPhone). Only
# enforce it when an agent is driving — Claude Code sets CLAUDECODE=1 in every
# command it spawns; a solo human developer runs lock-free. RGBSG_NO_LOCK=1
# forces the lock-free path even under an agent.
if [ -n "${CLAUDECODE:-}" ] && [ -z "${RGBSG_NO_LOCK:-}" ]; then
    if [ -x "$HW_LOCK" ]; then
        if ! "$HW_LOCK" check app; then
            echo "[!] Refusing to launch: the 'app' hardware lock is not held by this session." >&2
            echo "    Run: Monitor(command: \"scripts/hw-lock.sh hold app\", persistent: true) first (see the hw-lock skill)." >&2
            exit 1
        fi
        # Record our own pid (the exec below keeps it, so this becomes Metro's
        # real pid) so releasing the lock can stop Metro precisely — same
        # contract as launch-app.sh; see cmd_note_metro_pid in hw-lock.sh.
        if ! "$HW_LOCK" note-metro-pid app "$$"; then
            echo "[!] Refusing to launch: could not record this process as the 'app' lock's tracked Metro pid (the lock may have just been released) — re-acquire the lock and retry." >&2
            exit 1
        fi
    else
        echo "[!] scripts/hw-lock.sh not found — launching without the shared app lock." >&2
    fi
fi

cd "$REPO_ROOT/app"

# Defense 1: incremental prebuild sync so config-plugin output always lands,
# no matter how old the checked-out ios/ is. Cheap when nothing changed.
# --no-install skips pod install; `expo run:ios` runs it itself when needed.
echo "[*] Syncing ios/ prebuild (config plugins re-applied)..."
CI=1 npx expo prebuild --platform ios --no-install

# Defense 2: refuse to build unless the dev-variant bundle id actually landed.
# Glob the pbxproj — the project name varies with the tree's age (see header).
PBX=(ios/*.xcodeproj/project.pbxproj)
if [ ! -f "${PBX[0]}" ]; then
    echo "[!] No ios/*.xcodeproj/project.pbxproj after prebuild — prebuild failed?" >&2
    exit 1
fi
if ! grep -q 'PRODUCT_BUNDLE_IDENTIFIER = "com.autom8ed.rgbsunglassesapp.dev"' "${PBX[0]}"; then
    echo "[!] Refusing to build: ${PBX[0]} has no Debug-configuration" >&2
    echo "    PRODUCT_BUNDLE_IDENTIFIER = com.autom8ed.rgbsunglassesapp.dev — the" >&2
    echo "    withDevVariantIos plugin output is missing, so this build would install" >&2
    echo "    over the PRODUCTION app (this exact incident happened 2026-08-13)." >&2
    echo "    Try: rm -rf app/ios && re-run (full prebuild regenerate)." >&2
    exit 1
fi
echo "[*] Dev-variant bundle id verified in ${PBX[0]}"

exec npx expo run:ios "$@"
