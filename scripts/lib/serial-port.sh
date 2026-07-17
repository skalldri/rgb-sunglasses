#!/usr/bin/env bash
# serial-port.sh — shared helpers for finding the dev board's Zephyr shell UART
# from inside the devcontainer. Source this; don't execute it.
#
# The board (VID:PID 2fe3:0001) exposes the Zephyr shell on USB interface 00 and
# the MCUmgr/SMP UART transport on USB interface 02. ttyACM numbering shifts on
# every reboot/reflash, so ports are discovered by USB identity, never hardcoded.
# The devcontainer has no udev, so /dev/ttyACM* nodes sometimes need recreating
# by hand.
#
# When the board is held in MCUboot serial-recovery (DFU) mode instead of running
# the app, the app's 2fe3:0001 composite disappears and MCUboot brings up its own
# single CDC-ACM device (a different VID:PID) — serial_find_recovery_port finds it.
#
# Provides:
#   serial_recreate_dev_nodes   — mknod any missing /dev/ttyACM* nodes
#   serial_find_shell_port      — echo the app's Zephyr shell port, or return 1
#   serial_find_mcumgr_port     — echo the app's MCUmgr SMP port, or return 1
#   serial_find_recovery_port   — echo MCUboot's serial-recovery port, or return 1

# Recreate missing /dev/ttyACM* nodes (WSL/devcontainer has no udev).
serial_recreate_dev_nodes() {
  local d n majmin
  for d in /sys/class/tty/ttyACM*; do
    [ -e "$d" ] || continue
    n=$(basename "$d")
    majmin=$(cat "$d/dev")
    if [ ! -e "/dev/$n" ]; then
      mknod "/dev/$n" c "${majmin%%:*}" "${majmin##*:}"
      chmod 666 "/dev/$n"
    fi
  done
}

# Echo the ttyACM whose USB parent is the app dev board (2fe3:0001) on the given
# USB interface number ("00" = shell, "02" = MCUmgr). Returns 1 if not found.
# Recreates missing /dev nodes first.
_serial_find_board_iface() {
  local want_iface="$1"
  serial_recreate_dev_nodes
  local d ifdir usbdev vid pid ifnum
  for d in /sys/class/tty/ttyACM*; do
    [ -e "$d" ] || continue
    # Guarded: a stale sysfs entry must skip this candidate, not abort under set -e.
    ifdir=$(readlink -f "$d/device" 2>/dev/null) || continue   # .../<bus>-<port>:1.<iface>
    usbdev=$(readlink -f "$ifdir/.." 2>/dev/null) || continue
    vid=$(cat "$usbdev/idVendor" 2>/dev/null || echo "")
    pid=$(cat "$usbdev/idProduct" 2>/dev/null || echo "")
    ifnum=$(cat "$ifdir/bInterfaceNumber" 2>/dev/null || echo "")
    if [ "$vid" = "2fe3" ] && [ "$pid" = "0001" ] && [ "$ifnum" = "$want_iface" ]; then
      echo "/dev/$(basename "$d")"
      return 0
    fi
  done
  return 1
}

# Echo the app's Zephyr shell port (interface 00 of the 2fe3:0001 dev board), or
# return 1 if the board isn't present.
serial_find_shell_port() {
  _serial_find_board_iface "00"
}

# Echo the app's MCUmgr/SMP UART port (interface 02 of the 2fe3:0001 dev board),
# or return 1 if the board isn't present / hasn't finished booting.
serial_find_mcumgr_port() {
  _serial_find_board_iface "02"
}

# MCUboot serial-recovery CDC-ACM VID:PID. When the board is held in DFU/recovery
# mode, MCUboot enumerates its own single CDC-ACM device that is NOT the app's
# 2fe3:0001 composite. These values are authoritative — from MCUboot's build config
# (fw/sysbuild/mcuboot: CONFIG_USB_DEVICE_VID=0x2FE3 / CONFIG_USB_DEVICE_PID=0x0100,
# product string "MCUBOOT"; the Zephyr serial-recovery defaults). Override via env
# if a future MCUboot config changes them. If cleared, the finder falls back to the
# "lone non-app, non-J-Link CDC-ACM" heuristic below.
: "${RGBSG_RECOVERY_VID:=2fe3}"
: "${RGBSG_RECOVERY_PID:=0100}"

# Echo MCUboot's serial-recovery port, or return 1. Prefers an exact VID:PID
# match (RGBSG_RECOVERY_VID/PID); otherwise falls back to the single ttyACM whose
# USB parent is neither the app board (2fe3:0001) nor a J-Link (1366:xxxx). If the
# fallback is ambiguous (more than one candidate) it returns 1 so the caller can
# tell the user to pass --port explicitly rather than flash the wrong device.
serial_find_recovery_port() {
  serial_recreate_dev_nodes
  local d ifdir usbdev vid pid ifnum node
  local exact_if0="" exact_any=""          # exact VID:PID matches (prefer iface 00)
  local heur_if0="" heur_any=""            # heuristic-fallback ports
  local -a heur_devs=()                    # distinct non-app/non-J-Link USB devices
  for d in /sys/class/tty/ttyACM*; do
    [ -e "$d" ] || continue
    ifdir=$(readlink -f "$d/device" 2>/dev/null) || continue
    usbdev=$(readlink -f "$ifdir/.." 2>/dev/null) || continue
    vid=$(cat "$usbdev/idVendor" 2>/dev/null || echo "")
    pid=$(cat "$usbdev/idProduct" 2>/dev/null || echo "")
    ifnum=$(cat "$ifdir/bInterfaceNumber" 2>/dev/null || echo "")
    node="/dev/$(basename "$d")"

    # Exact recovery VID:PID match. MCUboot's recovery device exposes TWO CDC-ACM
    # interfaces (00 = the SMP recovery console, 02 = a second, dead CDC — hardware-
    # confirmed), so prefer interface 00 rather than trusting glob order.
    if [ -n "$RGBSG_RECOVERY_VID" ] && [ "$vid" = "$RGBSG_RECOVERY_VID" ] &&
       { [ -z "$RGBSG_RECOVERY_PID" ] || [ "$pid" = "$RGBSG_RECOVERY_PID" ]; }; then
      [ -z "$exact_any" ] && exact_any="$node"
      [ "$ifnum" = "00" ] && [ -z "$exact_if0" ] && exact_if0="$node"
      continue
    fi

    # Heuristic fallback: not the running app, not a J-Link VCOM.
    if [ "$vid" = "2fe3" ] && [ "$pid" = "0001" ]; then continue; fi   # app composite
    if [ "$vid" = "1366" ]; then continue; fi                          # J-Link VCOM
    local seen=0 x
    for x in ${heur_devs[@]+"${heur_devs[@]}"}; do
      [ "$x" = "$usbdev" ] && seen=1
    done
    [ "$seen" -eq 0 ] && heur_devs+=("$usbdev")
    [ -z "$heur_any" ] && heur_any="$node"
    [ "$ifnum" = "00" ] && [ -z "$heur_if0" ] && heur_if0="$node"
  done

  # Prefer an exact VID:PID match, interface 00 first.
  [ -n "$exact_if0" ] && { echo "$exact_if0"; return 0; }
  [ -n "$exact_any" ] && { echo "$exact_any"; return 0; }

  # Fallback: trust it only when exactly one non-app/non-J-Link USB device is present.
  if [ "${#heur_devs[@]}" -eq 1 ]; then
    [ -n "$heur_if0" ] && { echo "$heur_if0"; return 0; }
    [ -n "$heur_any" ] && { echo "$heur_any"; return 0; }
  fi
  return 1
}
