#!/usr/bin/env bash
# Provisions a connected board's NAND (external FAT) flash with the known
# GLIM animation assets and every compiled animation extension.
#
# Usage:
#   provision-device.sh                          # generate + push assets (fw/build)
#   provision-device.sh --build-dir <dir>        # override the build dir (default fw/build)
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

# Put the GLIM converters' python deps + yt-dlp/ffmpeg in reach before step 3
# runs them — no-op in the devcontainer, activates the tools venv on a macOS host
# (fails with instructions if neither is available; see scripts/tools-env.sh).
# Deliberately before any hardware work so a half-set-up host fails immediately
# instead of after mounting the board's disk.
. "$REPO_ROOT/scripts/tools-env.sh"

# 1. Locate the NAND USB mass-storage disk by SCSI vendor/product string
#    (USBD_DEFINE_MSC_LUN(nand, "NAND", "RGB-SG", "FlashDisk", "0.00") in
#    fw/src/usb/usb_init.c) rather than size or device-node name, both of
#    which shift depending on what else is attached. Both hosts match on the
#    same two strings, just through different registries.
HOST_OS="$(uname -s)"
DISK=""
case "$HOST_OS" in
    Linux)
        for dev in /sys/block/sd*; do
            [ -e "$dev/device/vendor" ] || continue
            vendor=$(tr -d ' \n' < "$dev/device/vendor" 2>/dev/null || true)
            model=$(tr -d ' \n' < "$dev/device/model" 2>/dev/null || true)
            if [ "$vendor" = "RGB-SG" ] && [ "$model" = "FlashDisk" ]; then
                DISK="/dev/$(basename "$dev")"
                break
            fi
        done
        ;;
    Darwin)
        # loginwindow "block[s] disk mounts during screen lock" (its own log
        # wording): with the console locked it dissents the mount and EJECTS the
        # disk ~2.7 s after it appears, and Zephyr's MSC latches the eject until
        # the board reboots. Root-caused on the Mac Mini 2026-08-13 (issue #367)
        # after this masqueraded as several other bugs — fail fast with the real
        # reason instead of "could not find the disk".
        if [ "$(ioreg -n Root -d1 -a 2>/dev/null | plutil -extract IOConsoleLocked raw - 2>/dev/null)" = "true" ]; then
            echo "[!] This Mac's screen is LOCKED — macOS loginwindow ejects the board's disk" >&2
            echo "    on sight while locked, so provisioning cannot reach it." >&2
            echo "    Unlock the screen, reboot the board (mcumgr reset), then re-run." >&2
            exit 1
        fi
        # IOKit names the published media "<vendor> <product> Media", so the
        # single string below pins vendor AND product the way the Linux branch
        # does with two files. Taking the BSD name from the IOMedia node (rather
        # than scraping `diskutil list`) also means we never guess a partition
        # suffix: the FAT volume IS the whole disk here (mkfs'd FM_SFD, no
        # partition table), so /dev/diskN is both the disk and the volume.
        # NB: awk must NOT `exit` on the first match — closing the pipe early
        # sends SIGPIPE to ioreg, and `set -o pipefail` then kills this script
        # with 141 before it prints anything. Flag the first hit instead.
        DISK_ID="$(ioreg -w0 -r -c IOMedia -l 2>/dev/null | awk '
            /RGB-SG FlashDisk Media/ { found = 1 }
            found && !got && /"BSD Name"/ { gsub(/^.*= "/, ""); gsub(/".*$/, ""); print; got = 1 }')"
        [ -n "$DISK_ID" ] && DISK="/dev/$DISK_ID"
        ;;
    *)
        echo "[!] Unsupported host OS: $HOST_OS (expected Linux or Darwin)." >&2
        exit 1
        ;;
esac

if [ -z "$DISK" ]; then
    echo "[!] Could not find the board's NAND USB mass-storage disk (vendor=RGB-SG, model=FlashDisk)." >&2
    echo "    Run /check-hardware and confirm the board is connected and enumerated." >&2
    if [ "$HOST_OS" = "Darwin" ]; then
        # Three distinct macOS-side causes, in likelihood order: (1) the display
        # dimmed — loginwindow's eject shield arms on dim WITHOUT setting
        # IOConsoleLocked, so the guard above can't catch it; (2) Zephyr's MSC is
        # still latched MEDIUM NOT PRESENT from an earlier macOS eject (clears on
        # board reboot); (3) a stale image that never publishes media at all.
        echo "    IOConsoleLocked was false, but note a DIMMED display arms the same eject" >&2
        echo "    shield without setting it (#367) — wake the display and retry first." >&2
        echo "    Otherwise the LUN is most likely still latched MEDIUM NOT PRESENT from an" >&2
        echo "    earlier macOS eject (reboot the board with mcumgr reset and re-run), or the" >&2
        echo "    board is carrying a stale image that never publishes media (reflash it)." >&2
        echo "    To see the SCSI state:" >&2
        echo "      /usr/bin/log show --last 5m --predicate 'eventMessage CONTAINS \"IOUSBMassStorageDriver\"'" >&2
    fi
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

# 4096 "greatest hits" (issue #96), and the canonical LZ4-compressed GLIM
# (format 4, Lz4PerFrameRgb24) — the first asset exercising the LZ4 decode path.
echo "[*] Generating 4096.glim (downloads source video, LZ4-compressed, ~1 min)..."
python3 "$REPO_ROOT/fw/tools/convert_video_to_glim.py" \
    --url "https://youtu.be/e9DfSCk-6Ko" --output "$TMP_GLIM/4096.glim" --fps 24 --lz4

# 4. Build every extension under fw/extensions/*/.
#    In a SUBSHELL that sources fw-env.sh, because extensions/build.sh needs west
#    on PATH (it regenerates the llext EDK) — a no-op in the devcontainer, the
#    ~/ncs venv on macOS. It has to be scoped: fw-env.sh and tools-env.sh
#    activate different python venvs and whichever is sourced last owns python3,
#    so leaking fw-env into this shell would break the converters above if the
#    steps were ever reordered.
echo "[*] Building extensions..."
( . "$REPO_ROOT/scripts/fw-env.sh" && "$REPO_ROOT/fw/extensions/build.sh" "$BUILD_DIR" )

# 5. Mount, copy, unmount.
#    macOS automounts removable FAT volumes, so there is no mount dir of ours to
#    create or clean up there — `diskutil mount` only has to cover the case where
#    the volume was ejected (which latches until the disk re-enumerates).
mount_fail_help() {
    echo "[!] The FAT filesystem may be corrupt or unformatted. Reformat it with the" >&2
    echo "    firmware's own 'fatfs reformat' shell command (over mcp__serial), reboot" >&2
    echo "    the board, then re-run this script — do not mkfs.vfat it from the host." >&2
}

if [ "$HOST_OS" = "Darwin" ]; then
    trap 'diskutil unmount "$DISK" >/dev/null 2>&1 || true; rm -rf "$TMP_GLIM"' EXIT

    MNT="$(diskutil info "$DISK" 2>/dev/null | awk -F':[[:space:]]*' '/^ *Mount Point/{print $2}')"
    if [ -z "$MNT" ]; then
        echo "[*] Mounting $DISK..."
        if ! diskutil mount "$DISK" >/tmp/provision-device-mount.log 2>&1; then
            echo "[!] Failed to mount $DISK:" >&2
            cat /tmp/provision-device-mount.log >&2
            mount_fail_help
            exit 1
        fi
        MNT="$(diskutil info "$DISK" 2>/dev/null | awk -F':[[:space:]]*' '/^ *Mount Point/{print $2}')"
    else
        echo "[*] $DISK already mounted at $MNT"
    fi
    if [ -z "$MNT" ]; then
        echo "[!] $DISK mounted but no mount point was reported." >&2
        exit 1
    fi
else
    MNT="$(mktemp -d /tmp/provision-mnt.XXXXXX)"
    trap 'umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true; rm -rf "$TMP_GLIM"' EXIT

    echo "[*] Mounting $DISK..."
    if ! mount -o rw "$DISK" "$MNT" 2>/tmp/provision-device-mount.log; then
        echo "[!] Failed to mount $DISK:" >&2
        cat /tmp/provision-device-mount.log >&2
        mount_fail_help
        exit 1
    fi
fi

mkdir -p "$MNT/glim" "$MNT/ext"

# macOS cp exports each source file's extended attributes (com.apple.provenance
# etc.) as an AppleDouble sidecar (._<name>) on FAT. Those names still end in
# .glim/.llext, so the firmware picks them up as real assets: hardware-verified
# that `glim list` showed ._4096.glim / ._bad_apple.glim / ._nyan_cat.glim
# alongside the real files, with the 4 KB sidecar ._4096.glim SELECTED as the
# active animation. (The extension registry rejects sidecars via manifest
# validation, so only GLIM is user-visibly broken — but both dirs are kept
# clean.) COPYFILE_DISABLE=1 does NOT fix this — it governs tar/copyfile, not
# cp; hardware-verified that sidecars are still written with it exported. BSD
# cp's -X ("do not copy extended attributes") is the real knob, plus a sweep
# AFTER the copy as a backstop (a pre-copy sweep just gets recreated).
if [ "$HOST_OS" = "Darwin" ]; then
    cp -X "$TMP_GLIM"/*.glim "$MNT/glim/"
    cp -X "$BUILD_DIR"/extensions/*.llext "$MNT/ext/"
    rm -f "$MNT"/glim/._* "$MNT"/ext/._*
else
    cp "$TMP_GLIM"/*.glim "$MNT/glim/"
    cp "$BUILD_DIR"/extensions/*.llext "$MNT/ext/"
fi
sync

echo "[*] Provisioned:"
ls -la "$MNT/glim"
ls -la "$MNT/ext"

# Unmount so the firmware's own FAT mount is the only writer again — the reboot
# below is what makes the board see the new files (see fw/CLAUDE.md on FAT
# concurrent access).
if [ "$HOST_OS" = "Darwin" ]; then
    diskutil unmount "$DISK" >/dev/null
else
    umount "$MNT"
fi
echo "[*] Done. Reboot the board (mcumgr reset, or a physical reset) so the" \
     "firmware re-mounts FAT and discovers the new files."

if [ "$HOST_OS" = "Darwin" ]; then
    echo "[*] macOS note: if the Mac's screen locks, loginwindow will eject the board's"
    echo "    disk the moment it re-appears (\"block disk mounts during screen lock\")."
    echo "    Unlock the screen and reboot the board to get the disk back."
fi
