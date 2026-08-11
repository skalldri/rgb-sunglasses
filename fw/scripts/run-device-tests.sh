#!/usr/bin/env bash
# run-device-tests.sh — build, flash and run the on-device (HIL) test suite
# against the attached proto0 board, via Zephyr Twister's device-testing mode.
#
# One entry point for all three consumers (human, AI agent, future CI runner):
#
#   fw/scripts/run-device-tests.sh                       # smoke + integration
#   fw/scripts/run-device-tests.sh --tier smoke          # just the 1-minute smoke pass
#   fw/scripts/run-device-tests.sh --tier destructive    # wipes + reprovisions the board
#   fw/scripts/run-device-tests.sh --tier dfu            # MCUmgr FW update loop (slow)
#   fw/scripts/run-device-tests.sh --standalone -k ibat  # fast inner loop, reuses fw/build
#   fw/scripts/run-device-tests.sh --build-only          # no hardware needed
#
# See fw/tests_device/README.md for tier semantics and fw/docs/on-device-testing.md
# for the architecture.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$REPO_ROOT/fw/scripts/lib/jlink.sh"

PLATFORM="rgb_sunglasses_proto0/nrf5340/cpuapp"
OUTDIR="$REPO_ROOT/fw/twister-device-out"
MAP_FILE=""
TIERS=()
KEXPR=""
BUILD_ONLY=0
TEST_ONLY=0
STANDALONE=0
STANDALONE_BUILD_DIR="$REPO_ROOT/fw/build"
LIST_ONLY=0
EXTRA_ARGS=()

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'
Flags:
  --tier T            smoke|integration|destructive|dfu|soak|all (repeatable).
                      Default: smoke + integration. "all" = every tier except
                      dfu and soak, which must always be requested explicitly.
  -k EXPR             pytest -k expression — --standalone only (under twister
                      the instance gets runtime-filtered and NOTHING runs;
                      hardware-observed). Use --tier/markers with twister.
  --build-only        build the twister test image(s), no hardware touched
  --test-only         re-run against existing twister artifacts (no rebuild)
  --standalone        bypass twister: run pytest directly against an existing
                      build dir (default fw/build) on the already-attached
                      board. Flashes once per session, then iterates fast.
  --build-dir DIR     build dir for --standalone (default fw/build)
  --outdir DIR        twister output dir (default fw/twister-device-out)
  --map FILE          use an existing hardware-map YAML instead of generating
  --list              list selected tests without running them
  -- ARGS...          forwarded to twister (or pytest in --standalone) verbatim
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --tier) TIERS+=("$2"); shift 2 ;;
        -k) KEXPR="$2"; shift 2 ;;
        --build-only) BUILD_ONLY=1; shift ;;
        --test-only) TEST_ONLY=1; shift ;;
        --standalone) STANDALONE=1; shift ;;
        --build-dir) STANDALONE_BUILD_DIR="$2"; shift 2 ;;
        --outdir) OUTDIR="$2"; shift 2 ;;
        --map) MAP_FILE="$2"; shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) echo "[!] Unknown flag: $1 (see --help)" >&2; exit 2 ;;
    esac
done

# This script is Linux/devcontainer-only, same as jlink-flash.sh (twister's
# device mode + the J-Link runner; native_sim tests are separate — /test-fw).
if [ "$(uname -s)" = "Darwin" ]; then
    echo "[!] run-device-tests.sh does not support macOS (needs the J-Link toolchain)." >&2
    exit 1
fi

# ---- tier -> pytest marker expression ------------------------------------------

if [ "${#TIERS[@]}" -eq 0 ]; then
    MARKERS="smoke or integration"
else
    parts=()
    for t in "${TIERS[@]}"; do
        case "$t" in
            smoke|integration|destructive|dfu|soak) parts+=("$t") ;;
            all) parts+=("smoke" "integration" "destructive") ;;
            *) echo "[!] Unknown tier: $t" >&2; exit 2 ;;
        esac
    done
    MARKERS=$(printf '%s or ' "${parts[@]}")
    MARKERS="${MARKERS% or }"
fi

# dfu/soak have their own twister scenarios (own timeout/slow gates); everything
# else runs inside app.device.hil. Mixing them in one invocation would need two
# scenarios anyway, so keep the selection honest:
SCENARIO="app.device.hil"
case " $MARKERS " in
    *" dfu "*)  SCENARIO="app.device.dfu" ;;
    *" soak "*) SCENARIO="app.device.soak" ;;
esac
if { [[ "$MARKERS" == *dfu* ]] || [[ "$MARKERS" == *soak* ]]; } && [[ "$MARKERS" == *" or "* ]]; then
    echo "[!] dfu/soak cannot be combined with other tiers in one run (separate twister scenarios)." >&2
    exit 2
fi

# ---- hardware lock gate ---------------------------------------------------------
# Same convention as jlink-flash.sh: enforced only when an agent is driving
# (CLAUDECODE set), check-only — this script never acquires the lock itself.
if [ "$BUILD_ONLY" -eq 0 ] && [ -n "${CLAUDECODE:-}" ] && [ -z "${RGBSG_NO_LOCK:-}" ]; then
    if ! "$REPO_ROOT/scripts/hw-lock.sh" check board; then
        echo "[!] Refusing to touch hardware: the 'board' hw-lock is not held by this session." >&2
        echo "    Run: Monitor(command: \"scripts/hw-lock.sh hold board\", persistent: true)" >&2
        exit 1
    fi
fi

# -k is only honored by the standalone pytest path; under twister the
# instance ends up runtime-filtered and the run reports success having
# executed zero tests — refuse rather than green-wash.
if [ -n "$KEXPR" ] && [ "$STANDALONE" -eq 0 ]; then
    echo "[!] -k requires --standalone (a twister run with -k executes nothing)." >&2
    exit 2
fi

# ---- standalone fast path -------------------------------------------------------

if [ "$STANDALONE" -eq 1 ]; then
    [ -d "$STANDALONE_BUILD_DIR" ] || { echo "[!] Build dir not found: $STANDALONE_BUILD_DIR (run /build-proto0 first)" >&2; exit 1; }
    "$REPO_ROOT/fw/scripts/fix-usb-dev-nodes.sh" || true
    SN=$(jlink_find_serial) || exit 1
    ZEPHYR_BASE="${ZEPHYR_BASE:-/root/ncs/v3.1.1/zephyr}"
    export PYTHONPATH="$ZEPHYR_BASE/scripts/pylib/pytest-twister-harness/src${PYTHONPATH:+:$PYTHONPATH}"
    cmd=(python3 -m pytest -p twister_harness.plugin --twister-harness
         "$REPO_ROOT/fw/tests_device"
         --device-type=hardware
         --device-serial-pty="python3 $REPO_ROOT/fw/scripts/tty-bridge.py"
         --build-dir="$STANDALONE_BUILD_DIR"
         --platform="$PLATFORM"
         --runner=jlink --device-id="$SN"
         --dut-scope=session
         -m "$MARKERS" -v)
    [ -n "$KEXPR" ] && cmd+=(-k "$KEXPR")
    [ "$LIST_ONLY" -eq 1 ] && cmd+=(--collect-only -q)
    cmd+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})
    echo "[*] standalone: ${cmd[*]}"
    exec "${cmd[@]}"
fi

# ---- twister path ---------------------------------------------------------------

if [ "$BUILD_ONLY" -eq 0 ]; then
    "$REPO_ROOT/fw/scripts/fix-usb-dev-nodes.sh" || true
    if [ -z "$MAP_FILE" ]; then
        SN=$(jlink_find_serial) || exit 1
        # NOT inside $OUTDIR: twister renames a pre-existing outdir to
        # <outdir>.N at startup, which would take the map with it.
        MAP_FILE=$(mktemp /tmp/rgbsg-hw-map.XXXXXX.yml)
        cat > "$MAP_FILE" <<EOF
- connected: true
  id: "$SN"
  platform: $PLATFORM
  product: rgbsg-proto0
  runner: jlink
  baud: 115200
  serial_pty: "python3,$REPO_ROOT/fw/scripts/tty-bridge.py"
  fixtures:
    - rgbsg_proto0
EOF
        echo "[*] Generated hardware map: $MAP_FILE (J-Link S/N $SN)"
    fi
fi

# --disable-warnings-as-errors: the production `west build` does not build
# with -Werror, and the proto0 sysbuild has documented-accepted warnings
# (fw/CLAUDE.md "Known non-blocking build warnings", e.g. the upstream
# USBD_CDC_ACM_LOG_LEVEL #warning). The test image must match production.
cmd=(twister
     -T "$REPO_ROOT/fw"
     -A "$REPO_ROOT/fw/boards"
     -x=BOARD_ROOT="$REPO_ROOT/fw"
     --outdir "$OUTDIR"
     -s "$SCENARIO"
     --disable-warnings-as-errors
     -v)

if [ "$BUILD_ONLY" -eq 1 ]; then
    cmd+=(-p "$PLATFORM" --build-only)
else
    # --west-flash is load-bearing: a sysbuild device test is silently SKIPPED
    # without it (zephyr twisterlib/runner.py).
    cmd+=(--device-testing --hardware-map "$MAP_FILE" --west-flash --device-flash-timeout 180)
fi
[ "$TEST_ONLY" -eq 1 ] && cmd+=(--test-only)
case "$SCENARIO" in app.device.dfu|app.device.soak) cmd+=(--enable-slow) ;; esac

# Marker/keyword filters ride through to the pytest child. Twister appends
# these AFTER the YAML's own pytest_args, and pytest's last -m/-k wins.
cmd+=(--pytest-args=-m --pytest-args="$MARKERS")
[ -n "$KEXPR" ] && cmd+=(--pytest-args=-k --pytest-args="$KEXPR")
[ "$LIST_ONLY" -eq 1 ] && cmd+=(--pytest-args=--collect-only --pytest-args=-q)
cmd+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

echo "[*] ${cmd[*]}"
cd "$REPO_ROOT"
exec "${cmd[@]}"
