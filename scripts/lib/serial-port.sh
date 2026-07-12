#!/usr/bin/env bash
# serial-port.sh — shared helpers for finding the dev board's Zephyr shell UART
# from inside the devcontainer. Source this; don't execute it.
#
# The board (VID:PID 2fe3:0001) exposes the Zephyr shell on USB interface 00.
# ttyACM numbering shifts on every reboot/reflash, so the port is discovered by
# USB identity, never hardcoded. The devcontainer has no udev, so /dev/ttyACM*
# nodes sometimes need recreating by hand.
#
# Provides:
#   serial_recreate_dev_nodes   — mknod any missing /dev/ttyACM* nodes
#   serial_find_shell_port      — echo the shell port path, or return 1 if absent

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

# Echo the Zephyr shell port (interface 00 of the 2fe3:0001 dev board), or
# return 1 if the board isn't present. Recreates missing nodes first.
serial_find_shell_port() {
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
    if [ "$vid" = "2fe3" ] && [ "$pid" = "0001" ] && [ "$ifnum" = "00" ]; then
      echo "/dev/$(basename "$d")"
      return 0
    fi
  done
  return 1
}
