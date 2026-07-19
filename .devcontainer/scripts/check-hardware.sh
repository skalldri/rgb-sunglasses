#!/usr/bin/env bash
# Checks for attached RGB Sunglasses dev board, J-Link, and Android device.
# Outputs a plain-text status report to stdout. Exit code is always 0.

BOARD_VID_PID="2fe3:0001"
JLINK_VID_PID="1366:0101"

echo "=== RGB Sunglasses Hardware Check ==="
echo ""

# ── macOS host ─────────────────────────────────────────────────────────────
# On a Mac (e.g. the Mac Mini with the board + an iPhone attached) there is no
# lsusb/sysfs and no adb phone; detection goes through the IORegistry and the
# phone is an iPhone via devicectl. This branch must stay bash-3.2-clean: the
# SessionStart hook runs this script with the stock macOS bash.
if [ "$(uname -s)" = "Darwin" ]; then
    REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
    # shellcheck source=../../scripts/lib/serial-port.sh
    . "$REPO_ROOT/scripts/lib/serial-port.sh"

    SHELL_PORT="$(serial_find_shell_port 2>/dev/null || true)"
    MCUMGR_PORT="$(serial_find_mcumgr_port 2>/dev/null || true)"

    # Note for both awk probes below: ioreg prints a node's properties
    # alphabetically (idProduct BEFORE idVendor), so v/p must be reset at every
    # node boundary ("+-o" line) — otherwise one node's leftover vendor can pair
    # with a later node's product and fire a false DETECTED.
    if [ -n "$SHELL_PORT" ] || [ -n "$MCUMGR_PORT" ] ||
       ioreg -p IOUSB -l 2>/dev/null | awk '
           /\+-o /          { v = ""; p = "" }
           /"idVendor" = /  { v = $NF }
           /"idProduct" = / { p = $NF }
           v == 12259 && p == 1 { found = 1 }
           END { exit !found }'; then
        echo "Dev board (2fe3:0001): DETECTED"
        if [ -n "$SHELL_PORT" ]; then
            echo "  $SHELL_PORT: OK  [Zephyr shell, 115200 baud]"
        else
            echo "  Zephyr shell port: NOT FOUND"
        fi
        if [ -n "$MCUMGR_PORT" ]; then
            echo "  $MCUMGR_PORT: OK  [MCUmgr firmware update, 115200 baud]"
        else
            echo "  MCUmgr port: NOT FOUND (board may still be booting)"
        fi
    else
        echo "Dev board (2fe3:0001): NOT DETECTED"
        echo "  Zephyr shell port: N/A"
        echo "  MCUmgr port: N/A"
    fi

    echo ""

    # J-Link: no SEGGER tooling is installed on the Mac; flashing here is via
    # MCUmgr OTA (fw/scripts/mcumgr-flash.sh) regardless. 4966/257 = 1366:0101.
    if ioreg -p IOUSB -l 2>/dev/null | awk '
           /\+-o /          { v = ""; p = "" }
           /"idVendor" = /  { v = $NF }
           /"idProduct" = / { p = $NF }
           v == 4966 && p == 257 { found = 1 }
           END { exit !found }'; then
        echo "J-Link (1366:0101): DETECTED  [unused on macOS — flashing is via MCUmgr OTA]"
    else
        echo "J-Link (1366:0101): NOT DETECTED  [normal on macOS — flashing is via MCUmgr OTA]"
    fi

    echo ""

    # Phone: on the Mac the companion-app device is an iPhone over devicectl.
    if command -v xcrun >/dev/null 2>&1; then
        # Match device rows by their CoreDevice UUID rather than assuming a
        # fixed-height header — devicectl's table layout is not a stable contract.
        IPHONES="$(xcrun devicectl list devices 2>/dev/null \
            | grep -E '[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}' || true)"
        if [ -n "$IPHONES" ]; then
            echo "iPhone (devicectl):"
            echo "$IPHONES" | sed 's/^/  /'
        else
            echo "iPhone (devicectl): NOT CONNECTED"
        fi
    else
        echo "iPhone (devicectl): N/A (xcrun not found — install Xcode)"
    fi

    echo ""
    echo "======================================"
    exit 0
fi

# ── Dev board ──────────────────────────────────────────────────────────────
if lsusb 2>/dev/null | grep -qi "$BOARD_VID_PID"; then
    echo "Dev board (2fe3:0001): DETECTED"

    # WSL2/udev sometimes fails to create /dev nodes for CDC-ACM interfaces even
    # though they appear in sysfs after a firmware reset. Create any that are missing.
    for sysdev in /sys/class/tty/ttyACM*; do
        node="/dev/$(basename "$sysdev")"
        if [ ! -e "$node" ]; then
            maj_min=$(cat "$sysdev/dev" 2>/dev/null) || continue
            maj=${maj_min%:*}; min=${maj_min#*:}
            mknod "$node" c "$maj" "$min" 2>/dev/null && chmod 666 "$node" || true
        fi
    done

    # Identify shell vs MCUmgr port from sysfs USB interface number.
    # The board always puts the Zephyr shell on USB interface x.0 and the MCUmgr
    # UART transport on interface x.2. Port numbers can shift after a reset.
    SHELL_PORT=""; MCUMGR_PORT=""
    for sysdev in /sys/class/tty/ttyACM*; do
        node="/dev/$(basename "$sysdev")"
        iface_path=$(readlink -f "$sysdev/device" 2>/dev/null)
        iface_suffix=$(echo "$iface_path" | grep -oE '\.[0-9]+$')
        case "$iface_suffix" in
            .0) SHELL_PORT="$node" ;;
            .2) MCUMGR_PORT="$node" ;;
        esac
    done

    if [ -n "$SHELL_PORT" ]; then
        echo "  $SHELL_PORT: OK  [Zephyr shell, 115200 baud]"
    else
        echo "  Zephyr shell port: NOT FOUND"
    fi

    if [ -n "$MCUMGR_PORT" ]; then
        echo "  $MCUMGR_PORT: OK  [MCUmgr firmware update, 115200 baud]"
    else
        echo "  MCUmgr port: NOT FOUND (board may still be booting)"
    fi

    if [ -z "$SHELL_PORT" ] && [ -z "$MCUMGR_PORT" ]; then
        echo "  [!] No usable TTYs found despite board being on USB."
        echo "  [!] Fix: run in a Windows terminal:"
        echo "  [!]   wsl -d docker-desktop -- modprobe cdc_acm"
        echo "  [!] Then unplug and replug the board."
    fi
else
    echo "Dev board (2fe3:0001): NOT DETECTED"
    echo "  Zephyr shell port: N/A"
    echo "  MCUmgr port: N/A"
fi

echo ""

# ── J-Link ─────────────────────────────────────────────────────────────────
if lsusb 2>/dev/null | grep -qi "$JLINK_VID_PID"; then
    echo "J-Link (1366:0101): DETECTED"

    # JLinkExe only opens the USB connection lazily, on the first command that
    # needs it - with just "Exit" in the command file it never connects at all,
    # so the probe must include a command (ShowHWStatus) that forces the connect.
    JLINK_OUT=$(printf 'ShowHWStatus\nExit\n' > /tmp/jlink_probe.jlink && \
                timeout 10 JLinkExe -CommandFile /tmp/jlink_probe.jlink 2>&1 || true)
    rm -f /tmp/jlink_probe.jlink

    if echo "$JLINK_OUT" | grep -q "Connecting to J-Link via USB...O.K."; then
        SERIAL=$(echo "$JLINK_OUT" | grep -oE 'S/N: [0-9]+' | grep -oE '[0-9]+' | head -n 1 || true)
        VTREF=$(echo "$JLINK_OUT" | grep -oE 'VTref=[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -n 1 || true)
        if [ -n "$VTREF" ]; then
            if awk "BEGIN{exit !($VTREF >= 3.0)}"; then
                echo "  Status: OK  [USB connected, VTref=${VTREF}V — target powered]"
            else
                echo "  Status: WARN  [USB connected, VTref=${VTREF}V — target voltage LOW]"
            fi
        else
            echo "  Status: OK  [USB connected, VTref not reported]"
        fi
        if [ -n "$SERIAL" ]; then
            echo "  Serial: $SERIAL  (use with: west flash --dev-id $SERIAL)"
        fi
    else
        echo "  Status: WARN  [USB present but J-Link Commander did not connect]"
    fi
else
    echo "J-Link (1366:0101): NOT DETECTED  [advanced programming unavailable]"
fi

echo ""

# WSL2/udev sometimes fails to create /dev/bus/usb nodes for a device even though it
# appears in sysfs (same class of problem as the ttyACM fixup above, but for any USB
# device — this hits phones too, not just the board/J-Link). Run the generalized
# fixup before probing for a phone below so a phone that's on the bus but has no
# node isn't misreported as "not connected" (issue #202).
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$REPO_ROOT/fw/scripts/fix-usb-dev-nodes.sh" >/dev/null 2>&1 || true

# The running adb server caches its device list from whenever it last enumerated
# /dev/bus/usb — a node created by the fixup above still won't show up in
# `adb devices` until the server restarts (experimentally confirmed: the fixup
# alone is silently ineffective without this). `adb kill-server` is enough; the
# next adb invocation below auto-restarts the server and re-scans.
adb kill-server >/dev/null 2>&1 || true

# ── Android (ADB) ──────────────────────────────────────────────────────────
# A device seen over TCP/WiFi stays on port 5555 permanently (set once via
# `adb tcpip 5555`). If it's seen on a different TCP port (e.g. after a fresh
# wireless-debug pairing), switch it to 5555 immediately so all future sessions
# connect the same way.
#
# NEVER run `adb tcpip` against a USB-attached device: it restarts the phone's
# adbd in TCP-only mode, which kills USB debugging outright and needs the
# phone's IP (or a phone-side toggle) to recover (issue #202). A USB-attached
# device's serial has no ":port" suffix — only switch a device that's already
# on TCP but on the wrong port.
ADB_STABLE_PORT=5555

_adb_serial() {
    adb devices 2>/dev/null | tail -n +2 | grep -v '^[[:space:]]*$' | grep 'device$' | awk '{print $1}' | head -n 1
}

_device_ip() {
    echo "$1" | cut -d: -f1
}

_is_tcp_serial() {
    case "$1" in
        *:*) return 0 ;;
        *) return 1 ;;
    esac
}

SERIAL=$(_adb_serial)

if [ -n "$SERIAL" ]; then
    if _is_tcp_serial "$SERIAL"; then
        DEVICE_IP=$(_device_ip "$SERIAL")
        DEVICE_PORT=$(echo "$SERIAL" | cut -d: -f2)

        # Not on the stable port — switch it now (one-time per pairing session).
        if [ "$DEVICE_PORT" != "$ADB_STABLE_PORT" ]; then
            adb -s "$SERIAL" tcpip "$ADB_STABLE_PORT" >/dev/null 2>&1 || true
            sleep 1
            adb connect "${DEVICE_IP}:${ADB_STABLE_PORT}" >/dev/null 2>&1 || true
            SERIAL=$(_adb_serial)
        fi
    fi

    if [ -n "$SERIAL" ]; then
        if _is_tcp_serial "$SERIAL"; then
            echo "Android (ADB): CONNECTED (WiFi) — $SERIAL"
        else
            echo "Android (ADB): CONNECTED (USB) — $SERIAL"
        fi
    else
        echo "Android (ADB): WARN — found device but failed to switch to port $ADB_STABLE_PORT"
        echo "  Try manually: adb connect ${DEVICE_IP}:${ADB_STABLE_PORT}"
    fi
else
    echo "Android (ADB): NOT CONNECTED"
    echo "  To connect wirelessly (no QR code — mDNS fails in-container):"
    echo "    1. Phone: Developer Options → Wireless debugging → Pair with pairing code"
    echo "    2. adb pair <IP:pairing-port>    (enter the 6-digit code)"
    echo "    3. adb connect <IP:debug-port>   (main port shown on Wireless debugging screen)"
    echo "  check-hardware will then switch the device to port $ADB_STABLE_PORT automatically."
fi

echo ""
echo "======================================"
exit 0
