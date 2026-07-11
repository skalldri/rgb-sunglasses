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

# shellcheck source=scripts/lib/serial-port.sh
source "$(cd "$(dirname "$0")" && pwd)/lib/serial-port.sh"

port=$(serial_find_shell_port) || {
  echo "error: no Zephyr shell port found — is the dev board (2fe3:0001) connected?" >&2
  exit 1
}

echo "Zephyr shell: $port @ 115200 (exit: Ctrl-A then k, then y)"
stty -F "$port" 115200 cs8 -cstopb -parenb raw -echo
exec screen "$port" 115200
