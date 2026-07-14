#!/usr/bin/env python3
"""Read-only power-register observation: periodically sample `power bq limits`
and `power pd contract` (+ `power policy` when built in) over the shell UART,
logging raw timestamped output. Used for the Experiment A captures in
docs/plans/power-management-overhaul.md.

Usage: observe-power.py [OUT_LOG] [--port /dev/ttyACM0] [--interval 20]
                        [--duration 600]

OWNS the UART for the whole run: hold the `board` hw-lock first, and close any
open MCP serial connection to the port (two readers race — see fw/CLAUDE.md).
"""
import argparse
import re
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("out", nargs="?", default="observe-power.log")
ap.add_argument("--port", default="/dev/ttyACM0")
ap.add_argument("--interval", type=int, default=20)
ap.add_argument("--duration", type=int, default=600)
args = ap.parse_args()

PORT = args.port
INTERVAL = args.interval
DURATION = args.duration
OUT = args.out

ANSI = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")


def run_cmd(ser, cmd):
    ser.reset_input_buffer()
    ser.write(b"\x03")  # clear line editor (boot-log fragments etc.)
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(cmd.encode() + b"\r\n")
    deadline = time.time() + 5
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            # done when we've seen the command echo AND a prompt after it
            text = ANSI.sub("", buf.decode(errors="replace"))
            if cmd in text and text.rstrip().endswith("uart:~$"):
                break
    return ANSI.sub("", buf.decode(errors="replace"))


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.3)
    samples = int(DURATION / INTERVAL)
    with open(OUT, "w") as f:
        for i in range(samples):
            stamp = time.strftime("%H:%M:%S")
            f.write(f"\n===== sample {i + 1}/{samples} at {stamp} =====\n")
            for cmd in ("power bq limits", "power pd contract", "power policy"):
                f.write(run_cmd(ser, cmd))
                f.write("\n")
            f.flush()
            if i < samples - 1:
                time.sleep(INTERVAL)
    ser.close()
    print(f"capture complete: {samples} samples -> {OUT}")


if __name__ == "__main__":
    main()
