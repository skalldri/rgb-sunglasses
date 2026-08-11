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
#   fw/scripts/run-device-tests.sh --standalone -k ibat  # fast inner loop (last twister build)
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
STANDALONE_BUILD_DIR=""
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
  --test-only         re-run against existing twister artifacts (no rebuild).
                      ONLY valid with the SAME --tier selection as the run
                      that produced them — twister reuses that run's test
                      plan, and a different tier executes zero tests while
                      reporting success (hardware-observed).
  --standalone        bypass twister: run pytest directly against an existing
                      build on the attached board. Defaults to the last
                      twister device build (fw/build's image has VT100 on,
                      which the harness cannot parse). Flashes once per
                      session, then iterates fast. Not combinable with
                      --build-only (standalone always flashes + runs).
  --build-dir DIR     build dir for --standalone (must have the fw/testcase.yaml
                      extra_configs; checked against its .config)
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
    parts=(smoke integration)
else
    parts=()
    for t in "${TIERS[@]}"; do
        case "$t" in
            smoke|integration|destructive|dfu|soak) parts+=("$t") ;;
            all) parts+=("smoke" "integration" "destructive") ;;
            *) echo "[!] Unknown tier: $t" >&2; exit 2 ;;
        esac
    done
fi

# Refuse a tier that would collect zero tests BEFORE spending 10+ minutes of
# build + flash on a locked board: an empty marker set makes pytest exit 5
# after the flash, which reads as a firmware regression (PR #341 review).
for t in "${parts[@]}"; do
    if ! grep -rqE "mark\.${t}\b" "$REPO_ROOT/fw/tests_device"/test_*.py; then
        echo "[!] Tier '${t}' has no tests yet (no @pytest.mark.${t} under fw/tests_device/)." >&2
        exit 2
    fi
done

MARKERS=$(printf '%s or ' "${parts[@]}")
MARKERS="${MARKERS% or }"

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

# --build-only is a twister-path concept. The standalone path ALWAYS flashes
# and runs — combining them previously bypassed the hw-lock gate while still
# driving the shared board (PR #341 review).
if [ "$STANDALONE" -eq 1 ] && [ "$BUILD_ONLY" -eq 1 ]; then
    echo "[!] --build-only has no meaning with --standalone (standalone always flashes + runs)." >&2
    exit 2
fi

# ---- standalone fast path -------------------------------------------------------

if [ "$STANDALONE" -eq 1 ]; then
    # Default to the LAST TWISTER DEVICE BUILD, not fw/build: the day-to-day
    # build has VT100 on and no crash-test commands — the one image the
    # harness cannot parse (PR #341 review). Run the twister path once (or
    # --build-only) to produce it, then iterate standalone against it.
    TWISTER_BUILD="$OUTDIR/rgb_sunglasses_proto0_nrf5340_cpuapp/zephyr/app.device.hil"
    if [ -z "$STANDALONE_BUILD_DIR" ]; then
        if [ -d "$TWISTER_BUILD" ]; then
            STANDALONE_BUILD_DIR="$TWISTER_BUILD"
        else
            echo "[!] No twister device build at $TWISTER_BUILD." >&2
            echo "    Run '$0 --build-only' first (or pass --build-dir for a suitably-configured build)." >&2
            exit 1
        fi
    fi
    [ -d "$STANDALONE_BUILD_DIR" ] || { echo "[!] Build dir not found: $STANDALONE_BUILD_DIR" >&2; exit 1; }

    # Fail fast on an image the harness is known unable to drive: the VT100
    # prompt-redraw escapes garble every captured line (see fw/testcase.yaml).
    APP_CONFIG="$STANDALONE_BUILD_DIR/fw/zephyr/.config"
    if [ ! -f "$APP_CONFIG" ]; then
        echo "[!] $APP_CONFIG missing — not a sysbuild app build dir." >&2
        exit 1
    fi
    if grep -q '^CONFIG_SHELL_VT100_COMMANDS=y' "$APP_CONFIG"; then
        echo "[!] $STANDALONE_BUILD_DIR was built with CONFIG_SHELL_VT100_COMMANDS=y —" >&2
        echo "    the pytest harness cannot parse that console. Use the twister device" >&2
        echo "    build (default when you omit --build-dir) or rebuild with the" >&2
        echo "    fw/testcase.yaml extra_configs." >&2
        exit 1
    fi

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
    # Probe the J-Link even when --map supplies the serial: a stale saved map
    # would otherwise only fail AFTER the multi-minute build, deep inside the
    # nrfutil runner with an opaque error (PR #341 review). The probe is the
    # immediate, actionable version of that failure.
    SN=$(jlink_find_serial) || exit 1
    if [ -z "$MAP_FILE" ]; then
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
# --test-only reuses the previous run's twister test plan, so it silently
# executes ZERO tests if the tier selection changed — refuse the mismatch
# instead of documenting it away (this trap was hit twice in one day).
MARKER_STAMP="$OUTDIR.last-markers"
if [ "$TEST_ONLY" -eq 1 ]; then
    if [ ! -f "$MARKER_STAMP" ] || [ "$(cat "$MARKER_STAMP")" != "$MARKERS" ]; then
        echo "[!] --test-only requires the same --tier selection as the previous run" >&2
        echo "    (previous: '$(cat "$MARKER_STAMP" 2>/dev/null || echo unknown)', requested: '$MARKERS')." >&2
        echo "    Drop --test-only to rebuild, or repeat the previous tier selection." >&2
        exit 2
    fi
    cmd+=(--test-only)
else
    printf '%s' "$MARKERS" > "$MARKER_STAMP"
fi
case "$SCENARIO" in app.device.dfu|app.device.soak) cmd+=(--enable-slow) ;; esac

# Marker/keyword filters ride through to the pytest child. Twister appends
# these AFTER the YAML's own pytest_args, and pytest's last -m/-k wins.
cmd+=(--pytest-args=-m --pytest-args="$MARKERS")
[ "$LIST_ONLY" -eq 1 ] && cmd+=(--pytest-args=--collect-only --pytest-args=-q)
cmd+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

echo "[*] ${cmd[*]}"
cd "$REPO_ROOT"
exec "${cmd[@]}"
