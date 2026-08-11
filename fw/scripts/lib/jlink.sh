#!/usr/bin/env bash
# jlink.sh — shared helper for probing the attached J-Link's serial number.
# Source this; don't execute it. Linux/devcontainer-only (lsusb + SEGGER tools),
# same constraint as jlink-flash.sh.
#
# Provides:
#   jlink_find_serial — echo the attached J-Link's serial number, or return 1
#                       (diagnostics on stderr; stdout carries only the S/N)

JLINK_VID_PID="1366:0101"

jlink_find_serial() {
  if ! lsusb 2>/dev/null | grep -qi "$JLINK_VID_PID"; then
    echo "[!] J-Link ($JLINK_VID_PID) not detected on USB. Run /check-hardware first." >&2
    return 1
  fi

  # JLinkExe only opens the USB connection lazily, on the first command that
  # needs it - ShowHWStatus forces the connect so the banner (incl. S/N) prints.
  local probe_file jlink_out serial
  probe_file=$(mktemp /tmp/jlink_probe.XXXXXX.jlink)
  printf 'ShowHWStatus\nExit\n' > "$probe_file"
  jlink_out=$(timeout 10 JLinkExe -CommandFile "$probe_file" 2>&1 || true)
  rm -f "$probe_file"

  if ! echo "$jlink_out" | grep -q "Connecting to J-Link via USB...O.K."; then
    echo "[!] USB present but J-Link Commander did not connect. Output:" >&2
    echo "$jlink_out" >&2
    return 1
  fi

  serial=$(echo "$jlink_out" | grep -oE 'S/N: [0-9]+' | grep -oE '[0-9]+' | head -n 1 || true)
  if [ -z "$serial" ]; then
    echo "[!] Connected to J-Link but could not parse serial number from output:" >&2
    echo "$jlink_out" >&2
    return 1
  fi

  echo "$serial"
}
