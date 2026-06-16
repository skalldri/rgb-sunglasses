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

    ACM0_OK=0; ACM1_OK=0
    [ -e /dev/ttyACM0 ] && ACM0_OK=1
    [ -e /dev/ttyACM1 ] && ACM1_OK=1

    if [ $ACM0_OK -eq 1 ]; then
        echo "  /dev/ttyACM0: OK  [Zephyr shell, 115200 baud]"
    else
        echo "  /dev/ttyACM0: MISSING"
    fi

    if [ $ACM1_OK -eq 1 ]; then
        echo "  /dev/ttyACM1: OK  [MCUmgr firmware update, 115200 baud]"
    else
        echo "  /dev/ttyACM1: MISSING"
    fi

    if [ $ACM0_OK -eq 0 ] || [ $ACM1_OK -eq 0 ]; then
        echo "  [!] TTYs missing despite board being on USB."
        echo "  [!] Fix: run in a Windows terminal:"
        echo "  [!]   wsl -d docker-desktop -- modprobe cdc_acm"
        echo "  [!] Then unplug and replug the board."
    fi
else
    echo "Dev board (2fe3:0001): NOT DETECTED"
    echo "  /dev/ttyACM0: N/A"
    echo "  /dev/ttyACM1: N/A"
fi

echo ""

# ── J-Link ─────────────────────────────────────────────────────────────────
if lsusb 2>/dev/null | grep -qi "$JLINK_VID_PID"; then
    echo "J-Link (1366:0101): DETECTED"

    JLINK_OUT=$(echo "Exit" > /tmp/jlink_probe.jlink && \
                timeout 10 JLinkExe -CommandFile /tmp/jlink_probe.jlink 2>&1 || true)
    rm -f /tmp/jlink_probe.jlink

    if echo "$JLINK_OUT" | grep -q "Connecting to J-Link via USB...O.K."; then
        VTREF=$(echo "$JLINK_OUT" | grep -oE 'VTref=[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' || true)
        if [ -n "$VTREF" ]; then
            if awk "BEGIN{exit !($VTREF >= 3.0)}"; then
                echo "  Status: OK  [USB connected, VTref=${VTREF}V — target powered]"
            else
                echo "  Status: WARN  [USB connected, VTref=${VTREF}V — target voltage LOW]"
            fi
        else
            echo "  Status: OK  [USB connected, VTref not reported]"
        fi
    else
        echo "  Status: WARN  [USB present but J-Link Commander did not connect]"
    fi
else
    echo "J-Link (1366:0101): NOT DETECTED  [advanced programming unavailable]"
fi

echo ""

# ── Android (ADB) ──────────────────────────────────────────────────────────
ADB_LINES=$(adb devices 2>/dev/null | tail -n +2 | grep -v '^[[:space:]]*$' || true)
CONNECTED=$(echo "$ADB_LINES" | grep 'device$' | awk '{print $1}' | tr '\n' ' ' | sed 's/ $//')

if [ -n "$CONNECTED" ]; then
    echo "Android (ADB): CONNECTED — $CONNECTED"
else
    echo "Android (ADB): NOT CONNECTED"
    echo "  To connect wirelessly (no QR code — mDNS fails in-container):"
    echo "    1. Phone: Developer Options → Wireless debugging → Pair with pairing code"
    echo "    2. adb pair <IP:pairing-port>    (enter the 6-digit code)"
    echo "    3. adb connect <IP:debug-port>   (main port shown on Wireless debugging screen)"
    echo "    4. adb devices                   (confirm)"
fi

echo ""
echo "======================================"
exit 0
