#!/usr/bin/env bash
# Debugs a fetched coredump with GDB via Zephyr's coredump gdbserver.
#
# The dump files coredump_manager writes to /NAND:/coredump (and
# coredump-fetch.sh copies off the board) are the raw Zephyr coredump binary
# stream ("ZE" header), which coredump_gdbserver.py consumes directly — no
# conversion needed. The gdbserver runs in --pipe mode as GDB's remote target,
# so there is no port to manage.
#
# Usage: coredump-debug.sh <core_NNNN.bin> [zephyr.elf]
#   The ELF must be the build that produced the crash (default:
#   <repo>/fw/build/fw/zephyr/zephyr.elf). Prints a backtrace on entry and
#   then drops into interactive GDB ('bt', 'info threads', 'frame N', etc.).
#
# For dumps captured over the serial log instead (`coredump print` /
# the old logging backend), first convert the #CD:-framed text:
#   python3 $NCS/zephyr/scripts/coredump/coredump_serial_log_parser.py <log> <out.bin>
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $(basename "$0") <core_NNNN.bin> [zephyr.elf]" >&2
    exit 1
fi

BIN="$1"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ELF="${2:-$REPO_ROOT/fw/build/fw/zephyr/zephyr.elf}"
NCS_ROOT="${NCS_ROOT:-/root/ncs/v3.1.1}"
GDBSERVER="$NCS_ROOT/zephyr/scripts/coredump/coredump_gdbserver.py"

if [ ! -f "$BIN" ]; then
    echo "[!] Dump file not found: $BIN" >&2
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "[!] ELF not found: $ELF (build the firmware first, or pass the ELF explicitly)" >&2
    exit 1
fi

# Sanity: raw Zephyr coredump streams start with the "ZE" magic.
if [ "$(head -c2 "$BIN")" != "ZE" ]; then
    echo "[!] $BIN does not start with the Zephyr coredump magic 'ZE'." >&2
    echo "    If this is a #CD:-framed serial log capture, convert it first with" >&2
    echo "    coredump_serial_log_parser.py (see the header of this script)." >&2
    exit 1
fi

# The toolchain hash directory varies per NCS install; glob it.
GDB=$(ls /root/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb 2>/dev/null | head -1)
if [ -z "$GDB" ]; then
    echo "[!] arm-zephyr-eabi-gdb not found under /root/ncs/toolchains/" >&2
    exit 1
fi

exec "$GDB" "$ELF" \
    -ex "target remote | python3 $GDBSERVER --pipe $ELF $BIN" \
    -ex "bt"
