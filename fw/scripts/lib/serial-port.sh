#!/usr/bin/env bash
# serial-port.sh — shared helpers for finding the dev board's Zephyr shell UART
# from inside the devcontainer or on a macOS host. Source this; don't execute it.
#
# The board (VID:PID 2fe3:0001) exposes the Zephyr shell on USB CDC-ACM function
# 0 and the MCUmgr/SMP UART transport on CDC-ACM function 1. Each function is a
# control+data interface pair, and the two OSes attach their tty to opposite
# halves: Linux's ttyACM binds to the CONTROL interfaces (00 shell / 02 MCUmgr,
# discovered via sysfs), while macOS's cu.usbmodem* binds to the DATA interfaces
# (1 shell / 3 MCUmgr, discovered via the IORegistry). Port names shift on every
# reboot/reflash on both OSes, so ports are discovered by USB identity, never
# hardcoded. The devcontainer additionally has no udev, so /dev/ttyACM* nodes
# sometimes need recreating by hand; macOS manages /dev itself.
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

_SERIAL_PORT_OS="$(uname -s)"

# --- macOS (Darwin) discovery --------------------------------------------------

# Emits one line per USB-backed serial port: "<vid_dec> <pid_dec> <iface> <dev>"
# (vendor/product in DECIMAL, as ioreg prints them; dev is the /dev/cu.* callout
# node). ioreg -r -t prints, for each IOSerialBSDClient, its full ancestor chain
# top-down, so the last idVendor/idProduct/bInterfaceNumber seen before the
# IOCalloutDevice line belong to that port's own USB device/interface; state is
# reset after each port so non-USB serial services (cu.debug-console, Bluetooth)
# can't inherit a previous port's identity.
_serial_darwin_usb_ports() {
  ioreg -r -t -c IOSerialBSDClient -l 2>/dev/null | awk '
    /"idVendor" = /         { v = $NF }
    /"idProduct" = /        { p = $NF }
    /"bInterfaceNumber" = / { b = $NF }
    /"IOCalloutDevice" = / {
      dev = $NF; gsub(/"/, "", dev)
      if (v != "" && p != "") print v, p, b, dev
      v = ""; p = ""; b = ""
    }
  '
}

# Darwin version of serial_find_recovery_port (same semantics: exact
# RGBSG_RECOVERY_VID/PID match preferred — data interface 1 first, the macOS
# face of the recovery SMP console on CDC function 0 — then the heuristic
# "lone USB serial device that is neither the app composite nor a J-Link",
# ambiguous ⇒ return 1). 12259 = 0x2fe3, 4966 = 0x1366.
_serial_darwin_find_recovery() {
  local rec_vid="" rec_pid="" vid pid ifn dev
  [ -n "$RGBSG_RECOVERY_VID" ] && rec_vid=$((16#$RGBSG_RECOVERY_VID))
  [ -n "$RGBSG_RECOVERY_PID" ] && rec_pid=$((16#$RGBSG_RECOVERY_PID))
  local exact_if1="" exact_any="" heur_if1="" heur_any=""
  local -a heur_devs=()
  while read -r vid pid ifn dev; do
    if [ -n "$rec_vid" ] && [ "$vid" = "$rec_vid" ] &&
       { [ -z "$rec_pid" ] || [ "$pid" = "$rec_pid" ]; }; then
      [ -z "$exact_any" ] && exact_any="$dev"
      [ "$ifn" = "1" ] && [ -z "$exact_if1" ] && exact_if1="$dev"
      continue
    fi
    if [ "$vid" = "12259" ] && [ "$pid" = "1" ]; then continue; fi   # app composite
    if [ "$vid" = "4966" ]; then continue; fi                        # J-Link VCOM
    local seen=0 x
    for x in ${heur_devs[@]+"${heur_devs[@]}"}; do
      [ "$x" = "$vid:$pid" ] && seen=1
    done
    [ "$seen" -eq 0 ] && heur_devs+=("$vid:$pid")
    [ -z "$heur_any" ] && heur_any="$dev"
    [ "$ifn" = "1" ] && [ -z "$heur_if1" ] && heur_if1="$dev"
  done < <(_serial_darwin_usb_ports)

  [ -n "$exact_if1" ] && { echo "$exact_if1"; return 0; }
  [ -n "$exact_any" ] && { echo "$exact_any"; return 0; }
  if [ "${#heur_devs[@]}" -eq 1 ]; then
    [ -n "$heur_if1" ] && { echo "$heur_if1"; return 0; }
    [ -n "$heur_any" ] && { echo "$heur_any"; return 0; }
  fi
  return 1
}

# Echo the callout device matching decimal VID $1, PID $2, interface $3, or
# return 1 if not present.
_serial_darwin_find() {
  local want_vid="$1" want_pid="$2" want_if="$3" vid pid ifn dev
  while read -r vid pid ifn dev; do
    if [ "$vid" = "$want_vid" ] && [ "$pid" = "$want_pid" ] && [ "$ifn" = "$want_if" ]; then
      echo "$dev"
      return 0
    fi
  done < <(_serial_darwin_usb_ports)
  return 1
}

# --- Linux (devcontainer/WSL) discovery ----------------------------------------

# Recreate missing /dev/ttyACM* nodes (WSL/devcontainer has no udev).
serial_recreate_dev_nodes() {
  [ "$_SERIAL_PORT_OS" = "Darwin" ] && return 0
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

# Echo the app's Zephyr shell port (CDC function 0 of the 2fe3:0001 dev board:
# Linux control interface 00 / macOS data interface 1), or return 1 if the board
# isn't present. 12259/1 = 0x2fe3/0x0001 in ioreg's decimal.
serial_find_shell_port() {
  if [ "$_SERIAL_PORT_OS" = "Darwin" ]; then
    _serial_darwin_find 12259 1 1
  else
    _serial_find_board_iface "00"
  fi
}

# Echo the app's MCUmgr/SMP UART port (CDC function 1 of the 2fe3:0001 dev board:
# Linux control interface 02 / macOS data interface 3), or return 1 if the board
# isn't present / hasn't finished booting.
serial_find_mcumgr_port() {
  if [ "$_SERIAL_PORT_OS" = "Darwin" ]; then
    _serial_darwin_find 12259 1 3
  else
    _serial_find_board_iface "02"
  fi
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
  if [ "$_SERIAL_PORT_OS" = "Darwin" ]; then
    _serial_darwin_find_recovery
    return
  fi
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
