#!/usr/bin/env bash
# Builds the RGB Sunglasses firmware with west + sysbuild — one command instead of
# the long west invocation. Compiles only; no hardware or hw-lock required.
#
# Usage:
#   fw/scripts/build-fw.sh                  # proto0 (default) -> fw/build
#   fw/scripts/build-fw.sh dk               # DK              -> fw/build-dk
#   fw/scripts/build-fw.sh --pristine       # force a clean (from-scratch) rebuild
#   fw/scripts/build-fw.sh proto0 -- <args> # args after -- are forwarded to west build
#
# The first build for a board is always a full pristine configure (it builds the
# netcore image, MCUboot, and the app), so it takes noticeably longer than the
# incremental builds after it. Use --pristine if a devicetree overlay or a
# board .conf fragment was newly ADDED (their cached paths gate re-discovery —
# see fw/CLAUDE.md, "Per-image Kconfig/devicetree overlays").
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD="proto0"
PRISTINE=()
EXTRA=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    proto0|dk)  BOARD="$1" ;;
    --pristine) PRISTINE=(--pristine) ;;
    --)         shift; EXTRA=("$@"); break ;;
    -h|--help)  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)          echo "[!] unknown argument: $1 (see --help)" >&2; exit 1 ;;
  esac
  shift
done

case "$BOARD" in
  proto0) BUILD_DIR="$REPO_ROOT/fw/build";    ZBOARD="rgb_sunglasses_proto0/nrf5340/cpuapp" ;;
  dk)     BUILD_DIR="$REPO_ROOT/fw/build-dk"; ZBOARD="rgb_sunglasses_dk/nrf5340/cpuapp" ;;
esac

# Run from the workspace/repo root (where the documented west invocation runs).
cd "$REPO_ROOT"

LABEL=""
[ "${#PRISTINE[@]}" -gt 0 ] && LABEL=" (pristine)"
echo "[*] Building $BOARD -> $BUILD_DIR$LABEL"
exec west build --build-dir "$BUILD_DIR" "$REPO_ROOT/fw" \
  --board "$ZBOARD" --sysbuild ${PRISTINE[@]+"${PRISTINE[@]}"} \
  -- -DBOARD_ROOT="$REPO_ROOT/fw" ${EXTRA[@]+"${EXTRA[@]}"}
