#!/usr/bin/env bash
# Fetches crash dumps (core_NNNN.bin) from the board's USB Mass Storage disk.
#
# The firmware's coredump_manager copies each captured coredump from the
# internal-flash coredump_partition to /NAND:/coredump/core_NNNN.bin (issue
# #80). This script finds the board's MSC disk (SCSI vendor "RGB-SG"), mounts
# it read-only (read-write with --delete), copies every dump file into the
# destination directory, and unmounts.
#
# Usage: coredump-fetch.sh [--delete] [dest-dir]
#   --delete   remove the dump files from the board after copying (stops the
#              firmware's periodic "awaiting collection" reminder). NOTE: the
#              firmware caches the FAT state it mounted at boot — reboot the
#              board after deleting so it re-reads the filesystem (see
#              fw/CLAUDE.md "FAT concurrent access causes read corruption").
#   dest-dir   where to copy the dumps (default: current directory)
#
# Debug a fetched dump with: coredump-debug.sh <core_NNNN.bin>
#
# Fallback when USB MSC isn't available: run `coredump print` on the Zephyr
# shell, save the #CD:-framed serial output to a file, and convert it with
#   python3 $NCS/zephyr/scripts/coredump/coredump_serial_log_parser.py <log> <out.bin>
set -euo pipefail

DELETE=0
DEST="."
for arg in "$@"; do
    case "$arg" in
        --delete) DELETE=1 ;;
        -*) echo "[!] Unknown option: $arg" >&2; exit 1 ;;
        *) DEST="$arg" ;;
    esac
done

# Identify the disk by its SCSI vendor/model strings, never a fixed /dev/sdX
# (the letter shifts based on what else is attached).
DISK=""
for dev in /sys/block/sd*; do
    [ -e "$dev" ] || continue
    vendor=$(cat "$dev/device/vendor" 2>/dev/null || true)
    if [[ "$vendor" == *"RGB-SG"* ]]; then
        DISK="/dev/$(basename "$dev")"
        break
    fi
done
if [ -z "$DISK" ]; then
    echo "[!] RGB-SG USB Mass Storage disk not found. Is the board plugged in and enumerated?" >&2
    echo "    (check: dmesg | grep RGB-SG ; lsblk)" >&2
    exit 1
fi
echo "[*] Found board disk: $DISK"

MNT=$(mktemp -d /tmp/sunglasses-fs.XXXXXX)
cleanup() {
    umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

if [ "$DELETE" = 1 ]; then
    mount -o rw "$DISK" "$MNT"
else
    mount -o ro "$DISK" "$MNT"
fi

shopt -s nullglob
DUMPS=("$MNT"/coredump/core_*.bin)
if [ "${#DUMPS[@]}" -eq 0 ]; then
    echo "[*] No crash dumps on the board."
    exit 0
fi

mkdir -p "$DEST"
for dump in "${DUMPS[@]}"; do
    cp -v "$dump" "$DEST/"
done

if [ "$DELETE" = 1 ]; then
    rm -v "${DUMPS[@]}"
    sync
    echo "[*] Dumps deleted from the board. Reboot the board before the firmware"
    echo "    touches /NAND:/coredump again (its FAT mount caches stale state)."
fi

echo "[*] Done. Debug with: $(dirname "$0")/coredump-debug.sh $DEST/$(basename "${DUMPS[-1]}")"
