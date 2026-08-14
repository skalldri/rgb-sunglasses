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
# This wrapper builds the DEBUG configuration, always: `--configuration Debug`
# is appended unconditionally and a caller-supplied `--configuration` with any
# other value is rejected — the Debug-scoped assert below can only vouch for
# Debug, and a Release build through this script would install under the
# PRODUCTION bundle id (the exact 2026-08-13 incident, via a different door).
# Release/TestFlight builds belong to app-release.yml, not this script.
#
# Usage: app/scripts/launch-app-ios.sh --device <UDID> [extra expo run:ios args]
#        app/scripts/launch-app-ios.sh --check-only   # prebuild + assert only, no build
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

# Parse args: pin the configuration to Debug (reject anything else, strip a
# redundant explicit Debug — it's re-appended below), and support --check-only
# (stop after the assert; lets the guard rails be exercised without a build).
# Array expansions use the ${arr[@]+...} guard throughout: this script runs on
# stock macOS bash 3.2, where an empty "$@"/array under `set -u` is fatal
# (bash < 4.4) — same idiom as fw/scripts/jlink-flash.sh.
CHECK_ONLY=0
PASS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check-only)
            CHECK_ONLY=1
            ;;
        --configuration)
            if [ "${2:-}" != "Debug" ]; then
                echo "[!] Refusing --configuration ${2:-<missing>}: this wrapper only builds Debug" >&2
                echo "    (the dev-variant assert below can only vouch for the Debug configuration;" >&2
                echo "    any other configuration installs under the PRODUCTION bundle id)." >&2
                echo "    Release/TestFlight builds go through app-release.yml." >&2
                exit 1
            fi
            shift # drop the value; --configuration Debug is re-appended below
            ;;
        --configuration=*)
            if [ "${1#*=}" != "Debug" ]; then
                echo "[!] Refusing ${1}: this wrapper only builds Debug (see --configuration note)." >&2
                exit 1
            fi
            ;;
        *)
            PASS+=("$1")
            ;;
    esac
    shift
done

# Defense 1: incremental prebuild sync so config-plugin output always lands,
# no matter how old the checked-out ios/ is. Cheap when nothing changed.
# --no-install skips pod install; `expo run:ios` runs it itself when needed.
echo "[*] Syncing ios/ prebuild (config plugins re-applied)..."
CI=1 npx expo prebuild --platform ios --no-install

# Defense 2: refuse to build unless the DEBUG configuration actually carries the
# dev-variant bundle id. Glob the pbxproj — the project name varies with the
# tree's age (see header) — and require exactly ONE match: with two projects
# (e.g. a pre-rename one beside a regenerated one, or a copy parked during
# recovery) we could assert one while xcodebuild builds the other.
PBX=(ios/*.xcodeproj/project.pbxproj)
if [ ! -f "${PBX[0]}" ]; then
    echo "[!] No ios/*.xcodeproj/project.pbxproj after prebuild — prebuild failed?" >&2
    exit 1
fi
if [ "${#PBX[@]}" -gt 1 ]; then
    echo "[!] Refusing to build: multiple Xcode projects under ios/ (${PBX[*]}) — cannot" >&2
    echo "    tell which one will be built. Try: rm -rf app/ios && re-run (full regenerate)." >&2
    exit 1
fi
# Scope the check to the Debug build-configuration blocks (the awk range covers
# each `/* Debug */ = { ... name = Debug; }` XCBuildConfiguration section), so a
# .dev id somewhere else in the file can't satisfy it. Two conditions: Debug
# MUST carry the .dev id, and Debug must NOT carry the bare production id.
DEBUG_IDS="$(awk '/\/\* Debug \*\/ = \{/,/name = Debug;/' "${PBX[0]}" | grep 'PRODUCT_BUNDLE_IDENTIFIER' || true)"
if ! printf '%s\n' "$DEBUG_IDS" | grep -q 'com\.autom8ed\.rgbsunglassesapp\.dev' \
   || printf '%s\n' "$DEBUG_IDS" | grep 'PRODUCT_BUNDLE_IDENTIFIER' | grep -vq '\.dev'; then
    echo "[!] Refusing to build: the Debug configuration in ${PBX[0]} does not carry" >&2
    echo "    PRODUCT_BUNDLE_IDENTIFIER com.autom8ed.rgbsunglassesapp.dev (found:" >&2
    printf '%s\n' "$DEBUG_IDS" | sed 's/^/      /' >&2
    echo "    ) — the withDevVariantIos plugin output is missing or wrong, so this build" >&2
    echo "    would install over the PRODUCTION app (this exact incident happened" >&2
    echo "    2026-08-13). Try: rm -rf app/ios && re-run (full prebuild regenerate)." >&2
    exit 1
fi
echo "[*] Dev-variant bundle id verified in the Debug configuration of ${PBX[0]}"

if [ "$CHECK_ONLY" = 1 ]; then
    echo "[*] --check-only: stopping before the build."
    exit 0
fi

exec npx expo run:ios --configuration Debug ${PASS[@]+"${PASS[@]}"}
