#!/usr/bin/env bash
# Provisions a connected board's NAND (external FAT) flash with the known
# GLIM animation assets and every compiled animation extension.
#
# Usage:
#   provision-device.sh                          # generate + push assets (fw/build)
#   provision-device.sh --build-dir fw/build-dk   # override the build dir (default fw/build)
#
# This script only does host-side, non-interactive work: locating and
# mounting the board's USB mass-storage disk, generating .glim files,
# building extensions, and copying everything over. It deliberately does NOT
# talk to the board's Zephyr shell (that must go through the mcp__serial__*
# MCP tools per fw/CLAUDE.md, not raw Bash) and does NOT reformat the FAT
# filesystem itself — a corrupt/unformatted disk must be rebuilt with the
# firmware's own `fatfs reformat` shell command (fw/src/storage/storage.cpp),
# not host-side mkfs.vfat, since the firmware owns the partition and that
# command is the one already documented/tested for this. The caller (the
# provision-device skill) is responsible for that device-side reformat and
# for rebooting the board afterwards so the firmware re-mounts FAT and
# discovers the new files.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/fw/build"

# The 'board' hw-lock coordinates the shared dev board across Claude Code agent
# worktrees (this writes to the board's NAND over USB mass storage and must not
# race with another agent flashing/resetting/talking to the board). Only enforce
# it when an agent is driving — Claude Code sets CLAUDECODE=1 in every command it
# spawns; a solo human developer provisions lock-free. RGBSG_NO_LOCK=1 forces the
# lock-free path.
if [ -n "${CLAUDECODE:-}" ] && [ -z "${RGBSG_NO_LOCK:-}" ]; then
    if ! "$REPO_ROOT/scripts/hw-lock.sh" check board; then
        echo "[!] Refusing to provision: the 'board' hardware lock is not held by this session." >&2
        echo "    Run: Monitor(command: \"scripts/hw-lock.sh hold board\", persistent: true)   (see the hw-lock skill)" >&2
        exit 1
    fi
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        *)
            echo "[!] Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# 1. Locate the NAND USB mass-storage disk by SCSI vendor/product string
#    (USBD_DEFINE_MSC_LUN(nand, "NAND", "RGB-SG", "FlashDisk", "0.00") in
#    fw/src/usb/usb_init.c) rather than size or /dev/sdX letter, both of
#    which shift depending on what else is attached.
DISK=""
for dev in /sys/block/sd*; do
    [ -e "$dev/device/vendor" ] || continue
    vendor=$(tr -d ' \n' < "$dev/device/vendor" 2>/dev/null || true)
    model=$(tr -d ' \n' < "$dev/device/model" 2>/dev/null || true)
    if [ "$vendor" = "RGB-SG" ] && [ "$model" = "FlashDisk" ]; then
        DISK="/dev/$(basename "$dev")"
        break
    fi
done

if [ -z "$DISK" ]; then
    echo "[!] Could not find the board's NAND USB mass-storage disk (vendor=RGB-SG, model=FlashDisk)." >&2
    echo "    Run /check-hardware and confirm the board is connected and enumerated." >&2
    exit 1
fi
echo "[*] Found NAND disk: $DISK"

# 2. Verify the firmware build exists before asking build.sh to regenerate
#    the LLEXT EDK against it.
if [ ! -f "$BUILD_DIR/fw/CMakeCache.txt" ]; then
    echo "[!] $BUILD_DIR is not a configured build directory." >&2
    echo "    Run /build-proto0 (or pass --build-dir) before provisioning." >&2
    exit 1
fi

# 3. Generate the known GLIM assets. This is the canonical set documented in
#    fw/CLAUDE.md ("Setting up GLIM files on a new board") — add another
#    line here if a new known asset is introduced.
TMP_GLIM="$(mktemp -d /tmp/provision-glim.XXXXXX)"
trap 'rm -rf "$TMP_GLIM"' EXIT

echo "[*] Generating nyan_cat.glim..."
python3 "$REPO_ROOT/fw/tools/generate_nyan_cat_glim.py" --output "$TMP_GLIM/nyan_cat.glim"

echo "[*] Generating bad_apple.glim (downloads source video, ~1 min)..."
python3 "$REPO_ROOT/fw/tools/convert_bad_apple.py" --output "$TMP_GLIM/bad_apple.glim"

# 4. Build every extension under fw/extensions/*/.
echo "[*] Building extensions..."
"$REPO_ROOT/fw/extensions/build.sh" "$BUILD_DIR"

# 5. Mount, copy, unmount.
MNT="$(mktemp -d /tmp/provision-mnt.XXXXXX)"
trap 'umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true; rm -rf "$TMP_GLIM"' EXIT

echo "[*] Mounting $DISK..."
if ! mount -o rw "$DISK" "$MNT" 2>/tmp/provision-device-mount.log; then
    echo "[!] Failed to mount $DISK:" >&2
    cat /tmp/provision-device-mount.log >&2
    echo "[!] The FAT filesystem may be corrupt or unformatted. Reformat it with the" >&2
    echo "    firmware's own 'fatfs reformat' shell command (over mcp__serial), reboot" >&2
    echo "    the board, then re-run this script — do not mkfs.vfat it from the host." >&2
    exit 1
fi
mkdir -p "$MNT/glim" "$MNT/ext"

cp "$TMP_GLIM"/*.glim "$MNT/glim/"
cp "$BUILD_DIR"/extensions/*.llext "$MNT/ext/"
sync

echo "[*] Provisioned:"
ls -la "$MNT/glim"
ls -la "$MNT/ext"

umount "$MNT"
echo "[*] Done. Reboot the board (mcumgr reset, or a physical reset) so the" \
     "firmware re-mounts FAT and discovers the new files."
