#!/usr/bin/env bash
# fw-shell.sh — interactive TTY to the board's Zephyr shell from inside the devcontainer.
#
# Auto-discovers the shell port (USB interface x.0 of the 2fe3:0001 composite device —
# ttyACM numbering shifts on every reboot/reflash, so never hardcode it), recreates any
# missing /dev nodes (no udev in this container), then attaches screen.
#
#   Detach & exit:  Ctrl-A then k  (then y)
#
# NOTE: do not run this while an mcp__serial__ connection is open to the same port —
# two readers race for the port and both see garbled data (see fw/CLAUDE.md).
set -euo pipefail

# Recreate missing /dev/ttyACM* nodes (WSL/devcontainer has no udev)
for d in /sys/class/tty/ttyACM*; do
  [ -e "$d" ] || continue
  n=$(basename "$d")
  majmin=$(cat "$d/dev")
  if [ ! -e "/dev/$n" ]; then
    mknod "/dev/$n" c "${majmin%%:*}" "${majmin##*:}"
    chmod 666 "/dev/$n"
  fi
done

# Find the Zephyr shell: interface 00 of the dev board (VID:PID 2fe3:0001)
port=""
for d in /sys/class/tty/ttyACM*; do
  [ -e "$d" ] || continue
  # Guarded: a stale sysfs entry must skip this candidate, not abort the scan (set -e)
  ifdir=$(readlink -f "$d/device" 2>/dev/null) || continue   # .../<bus>-<port>:1.<iface>
  usbdev=$(readlink -f "$ifdir/.." 2>/dev/null) || continue
  vid=$(cat "$usbdev/idVendor" 2>/dev/null || echo "")
  pid=$(cat "$usbdev/idProduct" 2>/dev/null || echo "")
  ifnum=$(cat "$ifdir/bInterfaceNumber" 2>/dev/null || echo "")
  if [ "$vid" = "2fe3" ] && [ "$pid" = "0001" ] && [ "$ifnum" = "00" ]; then
    port="/dev/$(basename "$d")"
    break
  fi
done

if [ -z "$port" ]; then
  echo "error: no Zephyr shell port found — is the dev board (2fe3:0001) connected?" >&2
  exit 1
fi

echo "Zephyr shell: $port @ 115200 (exit: Ctrl-A then k, then y)"
stty -F "$port" 115200 cs8 -cstopb -parenb raw -echo
exec screen "$port" 115200
