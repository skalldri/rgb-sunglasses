#!/usr/bin/env bash
# re-pair.sh — hands-off, fast BLE re-pairing of the shared Android phone to the
# nRF5340 dev board. Wraps the whole forget -> re-pair -> verify loop, with the
# time-critical passkey entry handled by a LOCAL autoresponder (serial tail ->
# instant ADB type), so it wins Android's pairing-dialog timeout race that a
# read-it-and-relay-it human/agent loop loses.
#
# WHY THIS EXISTS: on this non-spec-compliant OEM stack (OnePlus / OxygenOS), a
# GATT-changing reflash strands the phone with a stale bond+cache ("split-brain":
# `bt_state` shows CONNECTED/L4 but ATT MTU 23). The only fix is forget + re-pair,
# and the board displays a fresh RANDOM 6-digit passkey each time.
#
# LOCKS: touches the board (serial) AND the phone (adb), so it hard-refuses unless
# THIS session holds BOTH the `board` and `app` hardware locks (same check-only
# pattern as app/scripts/launch-app.sh; never acquires/releases them itself).
#
# SERIAL: it opens and OWNS the shell UART for the run. Close any open
# mcp__serial__ connection to that port first (serial_close) — two readers race
# and both see garbage (see fw/CLAUDE.md). Preflight aborts if the port is busy.
#
# Usage:
#   scripts/re-pair.sh [--serial <adb-serial>] [--device-name "RGB Sunglasses"]
#                      [--no-forget | --forget-only | --manual-forget]
#                      [--attempts N] [--keyevents] [--timeout-connect S]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)/.."
REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"
# shellcheck source=scripts/lib/serial-port.sh
source "$REPO_ROOT/scripts/lib/serial-port.sh"
HW_LOCK="$REPO_ROOT/scripts/hw-lock.sh"

# ---- defaults / args -------------------------------------------------------
PKG="com.autom8ed.rgbsunglassesapp.dev"
DEVICE_NAME="RGB Sunglasses"
ADB_SERIAL=""
FORGET_MODE="auto"        # auto | none | only | manual
ATTEMPTS=3
USE_KEYEVENTS=0
CONNECT_TIMEOUT=15        # seconds; matches the app's connectToDevice timeout

while [ $# -gt 0 ]; do
  case "$1" in
    --serial)          ADB_SERIAL="$2"; shift 2 ;;
    --device-name)     DEVICE_NAME="$2"; shift 2 ;;
    --no-forget)       FORGET_MODE="none"; shift ;;
    --forget-only)     FORGET_MODE="only"; shift ;;
    --manual-forget)   FORGET_MODE="manual"; shift ;;
    --attempts)        ATTEMPTS="$2"; shift 2 ;;
    --keyevents)       USE_KEYEVENTS=1; shift ;;
    --timeout-connect) CONNECT_TIMEOUT="$2"; shift 2 ;;
    -h|--help)         sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "[!] unknown arg: $1" >&2; exit 2 ;;
  esac
done

ADB=(adb)
[ -n "$ADB_SERIAL" ] && ADB=(adb -s "$ADB_SERIAL")

log()  { echo "[re-pair] $*"; }
warn() { echo "[re-pair] WARN: $*" >&2; }
die()  { echo "[re-pair] ERROR: $*" >&2; exit 1; }

# ---- cleanup ---------------------------------------------------------------
READER_PID=""
CAP=""
cleanup() {
  if [ -n "$READER_PID" ]; then kill "$READER_PID" 2>/dev/null || true; fi
  if [ -n "$CAP" ]; then rm -f "$CAP" 2>/dev/null || true; fi
  "${ADB[@]}" shell cmd statusbar collapse >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ===========================================================================
# Phase 0 — preflight
# ===========================================================================
preflight() {
  # Lock gate: both board and app. Check-only, never acquire (launch-app.sh pattern).
  if [ -x "$HW_LOCK" ]; then
    local missing=""
    "$HW_LOCK" check board >/dev/null 2>&1 || missing="board"
    "$HW_LOCK" check app   >/dev/null 2>&1 || missing="${missing:+$missing and }app"
    if [ -n "$missing" ]; then
      die "the '$missing' hardware lock is not held by this session. Hold both first:
    Monitor(command: \"scripts/hw-lock.sh hold board app\", persistent: true)"
    fi
  else
    warn "scripts/hw-lock.sh not found — proceeding without a lock check."
  fi

  # adb device present
  local state
  state="$("${ADB[@]}" get-state 2>/dev/null || true)"
  [ "$state" = "device" ] || die "no adb device (get-state='$state'). Check 'adb devices' / adb-connect.sh."

  # screen height (for band-based tap disambiguation)
  SCREEN_H="$("${ADB[@]}" shell wm size 2>/dev/null | sed -nE 's/.*: *[0-9]+x([0-9]+).*/\1/p' | tail -n1)"
  [ -n "${SCREEN_H:-}" ] || SCREEN_H=2412

  # serial port discovery + ownership
  PORT="$(serial_find_shell_port)" || die "no Zephyr shell port (board 2fe3:0001 not found)."
  if command -v fuser >/dev/null 2>&1 && fuser "$PORT" >/dev/null 2>&1; then
    die "$PORT is held by another process (an open mcp__serial__ connection, screen, or fw-shell.sh).
    Close it first: mcp__serial__serial_close, or stop that process. (pids: $(fuser "$PORT" 2>&1 | tr -s ' '))"
  fi
  log "shell UART: $PORT   screen height: ${SCREEN_H}px"

  # app present (relaunched in phase 3 anyway; warn only)
  "${ADB[@]}" shell pidof "$PKG" >/dev/null 2>&1 || warn "app '$PKG' not currently running (will relaunch)."
  if ! curl -sf --max-time 2 http://localhost:8081/status 2>/dev/null | grep -q packager-status:running; then
    warn "Metro not reporting running on :8081 — dev-client bundle may fail to load."
  fi
}

# ===========================================================================
# ADB / uiautomator helpers
# ===========================================================================
XML=""
ui_dump() {                     # refresh $XML from the current screen
  "${ADB[@]}" shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 || return 1
  "${ADB[@]}" exec-out cat /sdcard/ui.xml > "$XML" 2>/dev/null
  [ -s "$XML" ]
}

# ui_center <regex> [ymin_frac] [ymax_frac] -> echoes "x y" of a matching node's
# center (text or content-desc, case-insensitive), optionally within a vertical
# band (fraction of screen height). Returns 1 if none.
ui_center() {
  python3 - "$XML" "$1" "${2:-0}" "${3:-1}" "$SCREEN_H" <<'PY'
import sys, re, xml.etree.ElementTree as ET
xmlf, pat, ymin, ymax, H = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5])
try:
    root = ET.parse(xmlf).getroot()
except Exception:
    sys.exit(1)
rx = re.compile(pat, re.I)
for n in root.iter('node'):
    t = (n.get('text','') or '') + ' ' + (n.get('content-desc','') or '')
    if not rx.search(t):
        continue
    m = re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', n.get('bounds',''))
    if not m:
        continue
    x1,y1,x2,y2 = map(int, m.groups())
    cy = (y1+y2)//2
    if not (ymin*H <= cy <= ymax*H):
        continue
    print((x1+x2)//2, cy)
    sys.exit(0)
sys.exit(1)
PY
}

# ui_has <regex> -> 0 if a node matches (any text/content-desc/class)
ui_has() {
  python3 - "$XML" "$1" <<'PY'
import sys, re, xml.etree.ElementTree as ET
xmlf, pat = sys.argv[1], sys.argv[2]
try:
    root = ET.parse(xmlf).getroot()
except Exception:
    sys.exit(1)
rx = re.compile(pat, re.I)
for n in root.iter('node'):
    blob = ' '.join(filter(None, (n.get('text'), n.get('content-desc'), n.get('class'), n.get('resource-id'))))
    if rx.search(blob):
        sys.exit(0)
sys.exit(1)
PY
}

# ui_tap <regex> [ymin] [ymax] -> dump+find+tap. Returns 1 if not found.
ui_tap() {
  local xy
  ui_dump || return 1
  xy="$(ui_center "$1" "${2:-0}" "${3:-1}")" || return 1
  # shellcheck disable=SC2086
  "${ADB[@]}" shell input tap $xy
}

is_bonded() {                   # 0 if the board is in the phone's bonded list
  "${ADB[@]}" shell dumpsys bluetooth_manager 2>/dev/null \
    | sed -n '/Bonded devices/,/^$/p' | grep -qi "$DEVICE_NAME"
}

# ===========================================================================
# Phase 1 — forget the phone-side bond
# ===========================================================================
forget_bond() {
  [ "$FORGET_MODE" = "none" ] && { log "forget: skipped (--no-forget)."; return 0; }

  if ! is_bonded; then
    log "forget: board not in the phone's bonded list — nothing to forget."
    return 0
  fi

  if [ "$FORGET_MODE" != "manual" ]; then
    log "forget: driving Bluetooth settings to forget '$DEVICE_NAME'…"
    "${ADB[@]}" shell am start -a android.settings.BLUETOOTH_SETTINGS >/dev/null 2>&1 || true
    sleep 1.5
    local step
    for step in 1 2 3; do
      ui_dump || { sleep 1; continue; }
      # Tap the per-device gear/details (rightmost 'settings/details/info' node), else the row itself.
      if ! ui_tap 'settings|details|info|gear|configure' 0 0.95; then
        ui_tap "$DEVICE_NAME" 0 0.95 || true
      fi
      sleep 1
      if ui_tap 'forget|unpair'; then
        sleep 0.5
        ui_tap 'forget|unpair|ok' || true   # possible confirm dialog
        sleep 1
        is_bonded || { log "forget: bond cleared."; return 0; }
      fi
      warn "forget attempt $step didn't clear the bond; retrying…"
    done
    warn "automated forget failed — falling back to manual."
  fi

  # Manual fallback: verify by STATE (poll dumpsys), never trust a keypress.
  echo ""
  echo ">>> Please FORGET '$DEVICE_NAME' in the phone's Bluetooth settings now."
  echo ">>> (Settings > Bluetooth > the device's gear > Forget.)  Waiting up to 120s…"
  local waited=0
  while is_bonded; do
    sleep 2; waited=$((waited+2))
    [ "$waited" -ge 120 ] && die "bond still present after 120s — aborting."
  done
  log "forget: bond cleared (manual)."
}

# ===========================================================================
# Phase 2 — arm the serial watcher (owns the port)
# ===========================================================================
arm_serial() {
  CAP="$(mktemp "${TMPDIR:-/tmp}/re-pair.uart.XXXXXX")"
  stty -F "$PORT" 115200 cs8 -cstopb -parenb raw -echo
  cat "$PORT" >> "$CAP" &
  READER_PID=$!
  log "serial watcher armed (pid $READER_PID, capture $CAP)."
}

cap_off() { stat -c%s "$CAP"; }                       # current capture size
# scan_after <off> <regex> -> echoes the last matching line after byte <off>
scan_after() { tail -c +$(( $1 + 1 )) "$CAP" | grep -a "$2" | tail -n1; }
# passkey_after <off> -> echoes the 6-digit passkey, or empty
passkey_after() {
  tail -c +$(( $1 + 1 )) "$CAP" | grep -a 'Passkey for' \
    | sed -nE 's/.*Passkey for.*: ([0-9]{6}).*/\1/p' | tail -n1
}

# ===========================================================================
# Phase 3 — trigger pairing + passkey autoresponder (TIME-CRITICAL)
# ===========================================================================
relaunch_app() {
  log "relaunching app (clears any orphaned native BLE link)…"
  "${ADB[@]}" shell am force-stop "$PKG" >/dev/null 2>&1 || true
  sleep 1
  "${ADB[@]}" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
  sleep 4
  # Dismiss the auto-open App-Update modal ONLY if present (blind BACK would background the app).
  if ui_dump && ui_has 'update available|app-update|Download Update'; then
    log "dismissing app-update modal."
    "${ADB[@]}" shell input keyevent KEYCODE_BACK
    sleep 1
  fi
}

tap_connect() {                 # wait for the board row, tap the Connect AppButton
  local waited=0
  while [ "$waited" -lt 20 ]; do
    ui_dump && ui_has "$DEVICE_NAME" && break
    sleep 2; waited=$((waited+2))
  done
  ui_has "$DEVICE_NAME" || { warn "board '$DEVICE_NAME' not listed (advertising?)"; return 1; }
  # 'Connect' AppButton, excluding the bottom tab bar (bottom ~12%).
  ui_tap '^Connect$|Connect' 0 0.88
}

# answer_passkey <off> -> autoresponder loop. 0 on 'Pairing completed'.
answer_passkey() {
  local off="$1" deadline=$(( SECONDS + CONNECT_TIMEOUT )) pk="" entered=0 expanded=0
  while [ "$SECONDS" -lt "$deadline" ]; do
    [ -z "$pk" ] && pk="$(passkey_after "$off")"

    # Terminal UART signals
    [ -n "$(scan_after "$off" 'Pairing completed')" ] && { log "pairing completed."; return 0; }
    if [ -n "$(scan_after "$off" 'Pairing failed\|Pairing cancelled\|Disconnected (reason 19)')" ]; then
      warn "pairing failed/cancelled (UART)."; return 1
    fi

    ui_dump || { sleep 0.5; continue; }

    # Heads-up 'Pairing request' notification form -> open the full dialog.
    if ui_has 'pairing request' && ! ui_has 'class="android.widget.EditText"'; then
      ui_tap 'pair & connect|pairing request' || true
      sleep 0.5; continue
    fi
    # If nothing visible after a beat, pull the shade once as a backstop.
    if [ "$expanded" -eq 0 ] && ! ui_has 'pairing request|android.widget.EditText'; then
      "${ADB[@]}" shell cmd statusbar expand-notifications >/dev/null 2>&1 || true
      expanded=1; sleep 0.5; continue
    fi

    # Full dialog with a PIN field + known passkey -> enter it. The EditText usually has
    # no text/content-desc, so locate it by class and tap its bounds center directly.
    if [ "$entered" -eq 0 ] && [ -n "$pk" ] && ui_has 'class="android.widget.EditText"'; then
      local xy i d
      xy="$(python3 - "$XML" <<'PY'
import sys,re,xml.etree.ElementTree as ET
root=ET.parse(sys.argv[1]).getroot()
for n in root.iter('node'):
    if n.get('class')=='android.widget.EditText':
        m=re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]',n.get('bounds',''))
        if m:
            x1,y1,x2,y2=map(int,m.groups()); print((x1+x2)//2,(y1+y2)//2); break
PY
)"
      # shellcheck disable=SC2086
      [ -n "$xy" ] && "${ADB[@]}" shell input tap $xy
      sleep 0.3
      if [ "$USE_KEYEVENTS" -eq 1 ]; then
        for (( i=0; i<${#pk}; i++ )); do d="${pk:$i:1}"; "${ADB[@]}" shell input keyevent "KEYCODE_$d"; done
      else
        "${ADB[@]}" shell input text "$pk"
      fi
      entered=1; log "typed passkey $pk."
      sleep 0.3
      # Confirm.
      ui_tap '^pair$|^ok$|pair' 0 1 || "${ADB[@]}" shell input keyevent KEYCODE_ENTER
    fi
    sleep 0.4
  done
  warn "deadline (${CONNECT_TIMEOUT}s) reached without 'Pairing completed'."
  return 1
}

pair_once() {                   # one full connect+answer attempt
  relaunch_app
  local off; off="$(cap_off)"
  tap_connect || return 1
  answer_passkey "$off"
}

# ===========================================================================
# Phase 4 — verify (drives the shell over the owned port)
# ===========================================================================
verify() {
  local waited=0 off
  log "verifying via bt_state…"
  while [ "$waited" -lt 60 ]; do
    off="$(cap_off)"
    printf 'bt_state\r' > "$PORT"
    sleep 2; waited=$((waited+2))
    local snap; snap="$(tail -c +$(( off + 1 )) "$CAP")"
    if grep -qa 'ATT MTU: 23' <<<"$snap"; then
      warn "bt_state shows ATT MTU: 23 — split-brain persists (hard fail)."; return 1
    fi
    if grep -qa 'CONNECTED' <<<"$snap" && grep -qaE 'ATT MTU: ([0-9]{3,})' <<<"$snap"; then
      local mtu; mtu="$(grep -aoE 'ATT MTU: [0-9]+' <<<"$snap" | tail -n1)"
      grep -qa 'Security level: L4' <<<"$snap" && { log "verified: CONNECTED, L4, $mtu."; return 0; }
    fi
  done
  warn "bt_state did not reach a healthy CONNECTED/L4/MTU>23 within 60s."
  return 1
}

# ===========================================================================
# main
# ===========================================================================
preflight
forget_bond
[ "$FORGET_MODE" = "only" ] && { log "done (--forget-only)."; exit 0; }

arm_serial
ok=0
for (( attempt=1; attempt<=ATTEMPTS; attempt++ )); do
  log "=== pairing attempt $attempt/$ATTEMPTS ==="
  if pair_once; then ok=1; break; fi
  warn "attempt $attempt failed; retrying…"
  sleep 1
done
[ "$ok" -eq 1 ] || die "pairing did not complete after $ATTEMPTS attempts."

if verify; then
  log "SUCCESS — re-paired and verified (CONNECTED / L4 / MTU>23)."
  exit 0
else
  die "paired, but on-device verification failed (see bt_state above)."
fi
