#!/usr/bin/env bash
# Checks for attached RGB Sunglasses dev board, J-Link, and Android device.
# Outputs a plain-text status report to stdout. Exit code is always 0.

BOARD_VID_PID="2fe3:0001"
JLINK_VID_PID="1366:0101"

echo "=== RGB Sunglasses Hardware Check ==="
echo ""

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

# ── Android (ADB) ──────────────────────────────────────────────────────────
# Strategy: use `adb tcpip 5555` to pin a stable port on first connect.
# The random TLS-wireless-debugging port is only needed once; after we switch
# the device to TCP mode on 5555, every subsequent reconnect just does
# `adb connect <saved-ip>:5555` — no user interaction, no port hunting.
# Cache file stores just the device IP (not the port, which rotates).
ADB_CACHE_FILE="/workspaces/rgb-sunglasses/.adb_device"
ADB_STABLE_PORT=5555

_adb_serial() {
    # Returns the first connected device serial (ip:port), or empty.
    adb devices 2>/dev/null | tail -n +2 | grep -v '^[[:space:]]*$' | grep 'device$' | awk '{print $1}' | head -n 1
}

_device_ip() {
    # Extracts the IP from an adb serial like "192.168.1.34:39327" → "192.168.1.34"
    echo "$1" | cut -d: -f1
}

_pin_stable_port() {
    # Switches device to TCP mode on the stable port and reconnects there.
    local serial="$1"
    local ip
    ip=$(_device_ip "$serial")
    adb -s "$serial" tcpip "$ADB_STABLE_PORT" >/dev/null 2>&1 || return 1
    sleep 1
    adb connect "${ip}:${ADB_STABLE_PORT}" >/dev/null 2>&1 || true
}

SERIAL=$(_adb_serial)

# Not connected — try the saved IP on the stable port.
if [ -z "$SERIAL" ] && [ -f "$ADB_CACHE_FILE" ]; then
    SAVED_IP=$(tr -d '[:space:]' < "$ADB_CACHE_FILE")
    if [ -n "$SAVED_IP" ]; then
        adb connect "${SAVED_IP}:${ADB_STABLE_PORT}" >/dev/null 2>&1 || true
        sleep 0.5
        SERIAL=$(_adb_serial)
    fi
fi

if [ -n "$SERIAL" ]; then
    DEVICE_IP=$(_device_ip "$SERIAL")
    DEVICE_PORT=$(echo "$SERIAL" | cut -d: -f2)

    # If we're on a random TLS port, switch to the stable port now.
    if [ "$DEVICE_PORT" != "$ADB_STABLE_PORT" ]; then
        _pin_stable_port "$SERIAL" || true
        SERIAL=$(_adb_serial)
    fi

    # Save the IP for future reconnects.
    echo "$DEVICE_IP" > "$ADB_CACHE_FILE" 2>/dev/null || true
    echo "Android (ADB): CONNECTED — $SERIAL"
else
    echo "Android (ADB): NOT CONNECTED"
    if [ -f "$ADB_CACHE_FILE" ]; then
        SAVED_IP=$(tr -d '[:space:]' < "$ADB_CACHE_FILE")
        echo "  (Could not reach $SAVED_IP:$ADB_STABLE_PORT — phone may need wireless debugging re-enabled)"
        echo "  Once wireless debugging is on, run:  adb connect $SAVED_IP:<port shown on phone>"
        echo "  check-hardware will then pin port $ADB_STABLE_PORT automatically."
    else
        echo "  To connect wirelessly (no QR code — mDNS fails in-container):"
        echo "    1. Phone: Developer Options → Wireless debugging → Pair with pairing code"
        echo "    2. adb pair <IP:pairing-port>    (enter the 6-digit code)"
        echo "    3. adb connect <IP:debug-port>   (main port shown on Wireless debugging screen)"
        echo "  check-hardware will then pin port $ADB_STABLE_PORT for all future reconnects."
    fi
fi

echo ""
echo "======================================"
exit 0
