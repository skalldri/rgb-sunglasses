#!/usr/bin/env bash
#
# Recreate missing /dev/bus/usb/BBB/DDD character-device nodes (and the board's
# USB Mass Storage block node).
#
# Why this exists: the devcontainer (WSL2 docker-desktop) has no udev, so when a USB
# device re-enumerates (new devnum), no /dev node is created for it. Worse, a library
# that then open()s the expected path can leave a bogus 0-byte *regular file* there.
# The J-Link probe hits this constantly: its USB connection resets whenever the target
# re-enumerates or a flash is interrupted, after which JLinkExe reports
# "Cannot connect to J-Link" and nrfutil "Failed to open connection" even though lsusb
# shows the probe. (Same class of problem as the shifting ttyACM nodes documented in
# fw/CLAUDE.md, which check-hardware.sh already handles for tty devices.)
#
# The same missing-node problem hits the board's FAT "NAND" disk, which enumerates as
# a SCSI block device (/dev/sdX): without its node, coredump-fetch.sh and any manual
# mount fail with "special device /dev/sdX does not exist" even though the disk shows
# in /sys/block and lsblk.
#
# What it does:
#   - per USB device in sysfs: computes the expected node path
#     /dev/bus/usb/<busnum>/<devnum> and char major/minor (USB char major 189,
#     minor = (busnum-1)*128 + devnum-1); if the node is missing or not a character
#     device, (re)creates it with mknod
#   - for the board's Mass Storage disk (SCSI vendor "RGB-SG"): (re)creates its
#     /dev/sdX block node from the major:minor in sysfs
#   - finally prunes nodes under /dev/bus/usb whose devnum no longer exists in sysfs.
#
# Usage: fw/scripts/fix-usb-dev-nodes.sh   (needs root, which the devcontainer runs as)

set -euo pipefail

created=0
pruned=0

declare -A expected  # "BBB/DDD" -> 1

for dev in /sys/bus/usb/devices/*; do
    # Only real devices have busnum/devnum (interfaces like 1-1:1.0 do not)
    [ -f "$dev/busnum" ] && [ -f "$dev/devnum" ] || continue

    busnum=$(cat "$dev/busnum")
    devnum=$(cat "$dev/devnum")
    node=$(printf '/dev/bus/usb/%03d/%03d' "$busnum" "$devnum")
    minor=$(( (busnum - 1) * 128 + devnum - 1 ))
    expected[$(printf '%03d/%03d' "$busnum" "$devnum")]=1

    if [ ! -c "$node" ]; then
        # Missing entirely, or a bogus regular file left by a failed open(O_CREAT)
        rm -f "$node"
        mkdir -p "$(dirname "$node")"
        mknod "$node" c 189 "$minor"
        chmod 664 "$node"
        idv=$(cat "$dev/idVendor" 2>/dev/null || echo '????')
        idp=$(cat "$dev/idProduct" 2>/dev/null || echo '????')
        echo "created $node (c 189 $minor) for $idv:$idp"
        created=$((created + 1))
    fi
done

# Recreate the block node for the board's USB Mass Storage disk (the FAT "NAND"
# volume). Identify it by SCSI vendor "RGB-SG", never a fixed /dev/sdX letter — it
# shifts based on whatever else is attached. The volume has no partition table
# (blkid reports vfat on the whole disk), so only the whole-disk node is needed.
for bdev in /sys/block/sd*; do
    [ -e "$bdev" ] || continue
    vendor=$(cat "$bdev/device/vendor" 2>/dev/null || true)
    [[ "$vendor" == *"RGB-SG"* ]] || continue

    name=$(basename "$bdev")
    node="/dev/$name"
    maj_min=$(cat "$bdev/dev")   # "MAJ:MIN"
    maj=${maj_min%:*}
    min=${maj_min#*:}

    if [ ! -b "$node" ]; then
        # Missing entirely, or a bogus regular file left by a failed open(O_CREAT)
        rm -f "$node"
        mknod "$node" b "$maj" "$min"
        chmod 660 "$node"
        echo "created $node (b $maj $min) for RGB-SG Mass Storage disk"
        created=$((created + 1))
    fi
done

# Prune stale nodes for devnums that no longer exist (they accumulate on every
# re-enumeration and can confuse tools that scan the directory).
for node in /dev/bus/usb/[0-9][0-9][0-9]/[0-9][0-9][0-9]; do
    [ -e "$node" ] || continue
    key=${node#/dev/bus/usb/}
    if [ -z "${expected[$key]:-}" ]; then
        rm -f "$node"
        echo "pruned stale $node"
        pruned=$((pruned + 1))
    fi
done

echo "done: $created node(s) created, $pruned pruned"
