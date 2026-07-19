#!/usr/bin/env bash
# mcumgr-flash.sh — flash RGB Sunglasses firmware over MCUmgr/SMP serial, no J-Link.
#
# Two modes:
#   --app        (default) Update while the app is still running. Talks to the
#                firmware's own MCUmgr SMP server on the "MCUmgr" CDC-ACM port,
#                uploads the new image(s) to the secondary slot(s), marks them for
#                test, resets, and confirms after the board comes back.
#   --recovery   Recover a board that won't boot the app. Assumes the board is
#                already held in MCUboot serial-recovery (DFU) mode — on proto0,
#                hold the Left button (P1.11) for ~1 s while resetting/power-cycling.
#                Uploads directly to the recovery slot(s) via MCUboot's own SMP
#                console and resets. (MCUboot serial recovery does not stay up after
#                reset, so there is no test/confirm step in this mode.)
#
# Full human runbook (which button, how to tell you're in recovery, troubleshooting):
#   fw/docs/flashing-without-jlink.md  —  published at
#   https://rgb-sunglasses.autom8ed.com/recovery
#
# Usage:
#   fw/scripts/mcumgr-flash.sh [--app|--recovery] [--build-dir DIR]
#                              [--app-only] [--no-confirm] [--port DEV]
#
# Notes:
#   - MCUmgr/serial recovery updates APPLICATION images only (app + net core).
#     Reflashing MCUboot itself still needs a J-Link or the in-app mcuboot_update
#     path — see fw/CLAUDE.md.
#   - Uploading the app image over serial takes ~3-4 minutes; be patient.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=lib/serial-port.sh
source "$REPO_ROOT/fw/scripts/lib/serial-port.sh"

BAUD=115200
MODE="app"
BUILD_DIR="$REPO_ROOT/fw/build"
APP_ONLY=0
CONFIRM=1
PORT=""
# Per-request timeout (seconds) for mcumgr. Uploads stream many small requests;
# 40 s per request is generous. Override with RGBSG_MCUMGR_TIMEOUT.
MCUMGR_TIMEOUT="${RGBSG_MCUMGR_TIMEOUT:-40}"

# SMP packet size (connstring "mtu="). Larger frames cut per-packet round-trips.
# 768 is hardware-tuned: it raises the upload rate ~1.45x (≈2.7 -> ~3.9 KiB/s) and
# fits BOTH transports' receive buffers as-shipped — MCUboot serial recovery's stock
# 1024-byte CONFIG_BOOT_SERIAL_MAX_RECEIVE_SIZE and the app's 2048-byte
# CONFIG_MCUMGR_TRANSPORT_UART_MTU — so no bootloader reflash is needed. Do NOT raise
# it blindly: the rate is non-monotonic (mtu=700 measured SLOWER than 512, and >1024
# overflows the recovery buffer). ~3.9 KiB/s is a MCUboot per-byte floor that larger
# frames/windows don't beat. Override or disable (empty) with RGBSG_MCUMGR_MTU.
MCUMGR_MTU="${RGBSG_MCUMGR_MTU-768}"

die() { echo "[!] $*" >&2; exit 1; }
info() { echo "[*] $*"; }

# Build a serial connstring for a port, appending mtu= only when configured.
_connstr() {
  local s="dev=$1,baud=$BAUD"
  [ -n "$MCUMGR_MTU" ] && s="$s,mtu=$MCUMGR_MTU"
  printf '%s' "$s"
}

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

# ── Parse args ───────────────────────────────────────────────────────────────
while [ "$#" -gt 0 ]; do
  case "$1" in
    --app)       MODE="app" ;;
    --recovery)  MODE="recovery" ;;
    --app-only)  APP_ONLY=1 ;;
    --no-confirm) CONFIRM=0 ;;
    --build-dir) shift; BUILD_DIR="${1:?--build-dir needs a path}" ;;
    --port)      shift; PORT="${1:?--port needs a device path}" ;;
    -h|--help)   usage 0 ;;
    *)           die "unknown argument: $1 (see --help)" ;;
  esac
  shift
done

# A relative --build-dir is resolved against the repo root for convenience.
case "$BUILD_DIR" in
  /*) : ;;
  *)  BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac

# ── Resolve the mcumgr CLI ───────────────────────────────────────────────────
if ! command -v mcumgr >/dev/null 2>&1; then
  cat >&2 <<'EOF'
[!] The 'mcumgr' CLI was not found on PATH.
    - In the project dev container it is pre-installed at /usr/local/bin/mcumgr.
    - On a bare host, install Go and then:
        go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
        # ensure "$(go env GOPATH)/bin" (usually ~/go/bin) is on your PATH
EOF
  exit 1
fi

# ── Lock gate: required only when an agent (Claude Code) drives this ─────────
# Claude Code sets CLAUDECODE=1 in every command it spawns; a human shell does
# not. Agents share one physical board, so they must hold the 'board' hw-lock
# (see scripts/hw-lock.sh). Human end-users flashing their own board run
# lock-free. RGBSG_NO_LOCK=1 forces the lock-free path for edge cases.
if [ -n "${CLAUDECODE:-}" ] && [ -z "${RGBSG_NO_LOCK:-}" ]; then
  if ! "$REPO_ROOT/scripts/hw-lock.sh" check board >/dev/null 2>&1; then
    echo "[!] Refusing to flash: the 'board' hardware lock is not held by this session." >&2
    echo "    Run: Monitor(command: \"scripts/hw-lock.sh hold board\", persistent: true)   (see the hw-lock skill)" >&2
    echo "    (Human end-users outside an agent session are not affected by this check.)" >&2
    exit 1
  fi
fi

# ── Locate the images to upload ──────────────────────────────────────────────
# Prefer dfu_application.zip: it bundles the signed app + net-core images and a
# manifest.json that maps each file to its image index (app-mode -n) and its
# secondary slot index (recovery direct-upload -n). The slot indices come from
# the build's own manifest — never guessed — which is why recovery mode relies
# on the zip. If the zip is absent we fall back to the signed app image alone.
WORK_DIR=""
cleanup() { [ -n "$WORK_DIR" ] && rm -rf "$WORK_DIR"; }
trap cleanup EXIT

# Each entry: "<abs_file>|<image_index>|<secondary_slot>"
UPLOADS=()

DFU_ZIP="$BUILD_DIR/dfu_application.zip"
if [ -f "$DFU_ZIP" ]; then
  command -v unzip >/dev/null 2>&1 || die "found $DFU_ZIP but 'unzip' is not installed"
  WORK_DIR="$(mktemp -d)"
  unzip -q -o "$DFU_ZIP" -d "$WORK_DIR" || die "failed to unpack $DFU_ZIP"
  [ -f "$WORK_DIR/manifest.json" ] || die "$DFU_ZIP has no manifest.json"

  if command -v python3 >/dev/null 2>&1; then
    # Emit "<file>\t<image_index>\t<slot_index_primary>\t<slot_index_secondary>".
    while IFS=$'\t' read -r fname iidx pidx sidx; do
      [ -n "$fname" ] || continue
      [ -f "$WORK_DIR/$fname" ] || die "manifest references missing file: $fname"
      UPLOADS+=("$WORK_DIR/$fname|$iidx|$pidx|$sidx")
    done < <(python3 - "$WORK_DIR/manifest.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    m = json.load(f)
for e in m.get("files", []):
    print("\t".join([
        e.get("file", ""),
        str(e.get("image_index", "")),
        str(e.get("slot_index_primary", "")),
        str(e.get("slot_index_secondary", "")),
    ]))
PY
    )
  else
    die "python3 is required to read the DFU manifest; install it or pass a single image with --app-only against a build that exposes zephyr.signed.bin"
  fi
else
  # Fallback: the signed app image only (image 0, primary slot 1). No net core.
  APP_BIN="$BUILD_DIR/fw/zephyr/zephyr.signed.bin"
  [ -f "$APP_BIN" ] || die "no dfu_application.zip and no $APP_BIN — build the firmware first (see fw/CLAUDE.md)"
  UPLOADS+=("$APP_BIN|0|1|")
  info "dfu_application.zip not found — falling back to the app image only ($APP_BIN)"
  APP_ONLY=1
fi

# Serial recovery can only restore the APP core (written directly to its bootable
# primary slot). The network core cannot be recovered here: its real primary lives
# in inaccessible network-core flash and is updated only by MCUboot's swap+PCD path,
# which runs during a normal app-mode OTA — not from a direct recovery write. So
# recovery is always app-core-only; recover the app, let it boot (its USB MCUmgr
# server comes up independent of the net core), then run this script in app mode to
# update the net core.
if [ "$MODE" = "recovery" ] && [ "$APP_ONLY" -ne 1 ]; then
  APP_ONLY=1
  info "Recovery restores the app core only — update the net core via app-mode OTA after boot."
fi

# --app-only drops every non-image-0 entry (net core is image_index 1).
if [ "$APP_ONLY" -eq 1 ]; then
  filtered=()
  for u in "${UPLOADS[@]}"; do
    iidx="${u#*|}"; iidx="${iidx%%|*}"
    [ "$iidx" = "0" ] && filtered+=("$u")
  done
  # Empty-array-safe assignment (bash 3.2 on macOS errors on "${empty[@]}" under set -u).
  UPLOADS=(${filtered[@]+"${filtered[@]}"})
fi
[ "${#UPLOADS[@]}" -gt 0 ] || die "no images selected to upload"

# ── Resolve the serial port ──────────────────────────────────────────────────
if [ -z "$PORT" ]; then
  if [ "$MODE" = "recovery" ]; then
    PORT="$(serial_find_recovery_port || true)"
    [ -n "$PORT" ] || die "could not find a board in MCUboot serial-recovery mode.
    - Hold the Left button (P1.11) for ~1 s while resetting/power-cycling the board.
    - Then re-run, or pass the port explicitly with --port /dev/ttyACMx.
    - See fw/docs/flashing-without-jlink.md (Path B)."
  else
    PORT="$(serial_find_mcumgr_port || true)"
    [ -n "$PORT" ] || die "could not find the board's MCUmgr port (2fe3:0001 interface 02).
    - Is the board plugged in and running the app? Give it a few seconds after reset.
    - Run /check-hardware (agents) to list ports, or pass --port /dev/ttyACMx."
  fi
fi
[ -e "$PORT" ] || die "serial port does not exist: $PORT"

CONN=(--conntype serial --connstring "$(_connstr "$PORT")")
mcm() { mcumgr "${CONN[@]}" -t "$MCUMGR_TIMEOUT" "$@"; }

info "Mode: $MODE   Port: $PORT   Build: $BUILD_DIR"

# ── Connectivity check ───────────────────────────────────────────────────────
# MCUboot's serial recovery implements only a subset of SMP and does NOT support
# the OS `echo` command (hardware-confirmed: it NMP-times-out), so probe with
# `image list`, which both the running app and MCUboot recovery support.
info "Checking SMP connectivity..."
if ! mcm image list >/dev/null 2>&1; then
  die "no response from $PORT.
    - App mode: the app may not be running (try --recovery), or the port shifted (re-run).
    - Recovery mode: the board may not actually be in serial-recovery mode, or you may
      be on the wrong CDC port (the recovery console is interface 00). Try --port."
fi

# ── Upload ───────────────────────────────────────────────────────────────────
# App mode uploads to the image number (-n <image_index>); the firmware's SMP
# image manager routes it to that image's secondary slot.
#
# Recovery mode (MCUboot's CONFIG_MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD) writes the
# image directly to a slot by number. Critically, there is NO image-test/set-pending
# step in serial recovery, so an upload to the app SECONDARY slot is never installed
# by overwrite-only MCUboot on reset (hardware-confirmed: the board booted the OLD
# image). So recovery writes the app core to its PRIMARY slot (slot_index_primary),
# which is immediately bootable — the signed image carries its own MCUboot header, so
# it lands at the right offset. (Recovery is app-core-only; see the filter above.)
for u in "${UPLOADS[@]}"; do
  file="${u%%|*}"; rest="${u#*|}"
  iidx="${rest%%|*}"; rest="${rest#*|}"
  pidx="${rest%%|*}"
  if [ "$MODE" = "recovery" ]; then
    nsel="$pidx"; [ -n "$nsel" ] || die "no primary slot index for $(basename "$file") — recovery needs the DFU manifest"
  else
    nsel="${iidx:-0}"
  fi
  info "Uploading $(basename "$file") (-n $nsel) — this can take several minutes..."
  mcm image upload -n "$nsel" "$file" || die "upload of $(basename "$file") failed"
done

# ── Recovery mode: reset and we're done ──────────────────────────────────────
if [ "$MODE" = "recovery" ]; then
  info "Resetting the board out of recovery..."
  mcm reset || info "reset command returned non-zero — power-cycle the board manually if it doesn't reboot"
  info "Done — app core recovered. The board should boot the app now (give it ~15 s)."
  info "To also update the network core, once it has booted run app-mode OTA:"
  info "  fw/scripts/mcumgr-flash.sh    (no --recovery)"
  exit 0
fi

# ── App mode: mark uploaded images for test ──────────────────────────────────
# Parse `image list` to find each uploaded image's slot-1 (secondary) hash, then
# `image test <hash>` so MCUboot picks it up on the next boot.
if ! command -v python3 >/dev/null 2>&1; then
  info "python3 not available to auto-parse image hashes."
  info "Run manually:  mcumgr ${CONN[*]} image list   then   image test <hash>   then   reset"
  exit 0
fi

info "Reading image list to mark new images for test..."
LIST_OUT="$(mcm image list 2>/dev/null)" || die "image list failed"

# For each image number we uploaded, grab the slot=1 (secondary) hash. The image
# list text is passed via the environment (not interpolated into the source) so
# arbitrary output can't break the parser.
secondary_hash() {
  local want_image="$1"
  RGBSG_LIST="$LIST_OUT" python3 - "$want_image" <<'PY'
import os, sys, re
want = sys.argv[1]
text = os.environ.get("RGBSG_LIST", "")
cur_img = cur_slot = None
for line in text.splitlines():
    m = re.search(r'image=(\d+)\s+slot=(\d+)', line)
    if m:
        cur_img, cur_slot = m.group(1), m.group(2)
        continue
    m = re.search(r'hash:\s*([0-9a-fA-F]+)', line)
    if m and cur_img == want and cur_slot == "1":
        print(m.group(1)); break
PY
}

TESTED=0
for u in "${UPLOADS[@]}"; do
  rest="${u#*|}"; iidx="${rest%%|*}"; iidx="${iidx:-0}"
  hash="$(secondary_hash "$iidx" || true)"
  if [ -z "$hash" ]; then
    info "could not find a secondary-slot hash for image $iidx — skipping test (upload may not have landed)"
    continue
  fi
  info "Marking image $iidx (hash ${hash:0:16}...) for test..."
  mcm image test "$hash" || die "image test failed for image $iidx"
  TESTED=$((TESTED + 1))
done
[ "$TESTED" -gt 0 ] || die "no images were marked for test — nothing to boot"

# ── Reset and wait for the board to re-enumerate ─────────────────────────────
info "Resetting to apply the update..."
mcm reset || info "reset returned non-zero — reset the board manually if it doesn't reboot"

# This project builds MCUboot in overwrite-only mode
# (SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y): on reboot MCUboot copies the tested
# image from the secondary slot into the primary and boots it permanently. There
# is no test/revert window, so the image is auto-confirmed on copy and an explicit
# `image confirm` is unnecessary — on this hardware it actually returns EINVAL.
# We still attempt it best-effort (so this script keeps working if the bootloader
# is ever switched to a swap mode), and treat the app's MCUmgr port reappearing as
# the real success signal — that port only exists once the new app image boots.
# A multi-image (app + net-core) update plus USB re-enumeration can take well over
# 30 s, so wait longer. Override with RGBSG_REENUM_WAIT.
REENUM_WAIT="${RGBSG_REENUM_WAIT:-90}"
info "Waiting up to ${REENUM_WAIT}s for the board to reboot and re-enumerate..."
NEWPORT=""
for _ in $(seq 1 "$REENUM_WAIT"); do
  sleep 1
  NEWPORT="$(serial_find_mcumgr_port || true)"
  [ -n "$NEWPORT" ] && break
done
if [ -z "$NEWPORT" ]; then
  info "Board hasn't re-enumerated yet. The update was uploaded and marked for boot —"
  info "give it more time, then confirm it came back (e.g. /check-hardware, or mcumgr image list)."
  exit 0
fi

CONN=(--conntype serial --connstring "$(_connstr "$NEWPORT")")
if [ "$CONFIRM" -eq 1 ]; then
  # No-op (EINVAL) under overwrite-only; genuinely needed only in a swap mode.
  mcm image confirm >/dev/null 2>&1 || true
fi
info "Done — board rebooted on $NEWPORT running the updated firmware."
