#!/bin/sh
# Runs as devcontainer initializeCommand.
#
# When using "Clone Repository in Container Volume" (the Docker Desktop + VS Code flow),
# initializeCommand is executed inside the docker-desktop WSL2 distro via
#   wsl -d docker-desktop /bin/sh -c ...
# NOT on the Windows host. PowerShell is therefore unavailable here.
#
# docker-desktop IS the WSL2 kernel host, so modprobe works directly and affects all
# WSL2 distros + privileged Docker containers that share the same kernel.
#
# usbipd attach is attempted via Windows interop (usbipd.exe). Windows interop may be
# disabled in docker-desktop; if so, the attach step is skipped with a warning and the
# user can run it manually from Windows PowerShell (see USB.md).

HW_IDS="2fe3:0001 1366:0101"
MODULES="cdc-acm usb-storage vhci-hcd usbip-host"

log()  { printf '[usb-init] %s\n' "$*"; }
warn() { printf '[usb-init] WARNING: %s\n' "$*"; }

# --- 1. Load kernel modules ----------------------------------------------------------
log "loading USB kernel modules: $MODULES"
if modprobe -a $MODULES 2>/dev/null; then
    log "kernel modules loaded."
else
    warn "modprobe returned non-zero (modules may already be loaded, or unavailable in this environment)."
fi

# --- 2. usbipd auto-attach via Windows interop (best-effort) -------------------------
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
