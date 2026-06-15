#!/bin/sh
# Runs as devcontainer initializeCommand on whatever host shell VS Code invokes.
#
# NOTE: modprobe is NOT run here. Kernel modules (cdc-acm, usb-storage, vhci-hcd,
# usbip-host) are loaded persistently inside docker-desktop via /etc/wsl.conf [boot].
# Run .devcontainer/scripts/setup-wsl-modules.ps1 once from PowerShell to install that.
#
# usbipd attach is attempted via Windows interop (usbipd.exe). If unavailable, the
# user can run it manually from Windows PowerShell (see USB.md).

HW_IDS="2fe3:0001 1366:0101"

log()  { printf '[usb-init] %s\n' "$*"; }
warn() { printf '[usb-init] WARNING: %s\n' "$*"; }

# --- 1. usbipd auto-attach via Windows interop (best-effort) -------------------------
if command -v usbipd.exe >/dev/null 2>/dev/null; then
    log "Windows interop available; starting usbipd auto-attach..."
    for hwid in $HW_IDS; do
        usbipd.exe attach --wsl --auto-attach --hardware-id "$hwid" >/dev/null 2>&1 &
        log "usbipd auto-attach started for $hwid"
    done
    # Give the attach a moment before the container finishes booting.
    sleep 2
else
    warn "usbipd.exe not on PATH (Windows interop disabled or unavailable in docker-desktop)."
    warn "If USB devices aren't visible in the container, from Windows PowerShell run:"
    for hwid in $HW_IDS; do
        warn "  usbipd attach --wsl --auto-attach --hardware-id $hwid"
    done
    warn "See .devcontainer/USB.md for details."
fi

exit 0
