#!/usr/bin/env python3
"""tty-bridge.py — serial_pty bridge between Twister/pytest and the dev board.

Twister's device harness normally opens a /dev/ttyACMn path directly, but this
board's shell is a soft-USB CDC-ACM function served by the nRF5340 itself: it
disappears during every flash/reset and re-enumerates under a *different*
ttyACM minor (and the devcontainer has no udev, so the new /dev node may not
even exist). A direct path goes stale mid-run.

This script is the fix: the hardware map lists it as `serial_pty`, so the
harness talks to a PTY that never dies, while this bridge re-resolves the
board's shell tty underneath — by USB identity (VID 2fe3 / PID 0001,
bInterfaceNumber 00), never by node name — creating missing /dev nodes from
sysfs, and reconnecting forever whenever the device vanishes.

Protocol (pytest-twister-harness HardwareAdapter._open_serial_pty):
  - stdin  <- bytes the harness writes toward the device
  - stdout -> bytes read from the device
  - stderr -> ALSO captured as device output by the harness — never print
    diagnostics there; set RGBSG_BRIDGE_LOG=<file> to get a debug trail.
  - exit 0 on stdin EOF / SIGTERM (harness closing the session).
"""

import errno
import fcntl
import os
import select
import signal
import stat
import sys
import termios
import time

BOARD_VID = "2fe3"
BOARD_PID = "0001"
SHELL_IFACE = "00"  # CDC function 0 control interface = Zephyr shell (Linux)
BAUD = termios.B115200
RESOLVE_RETRY_S = 0.5

_log_file = None


def log(msg: str) -> None:
    global _log_file
    path = os.environ.get("RGBSG_BRIDGE_LOG")
    if not path:
        return
    if _log_file is None:
        _log_file = open(path, "a", buffering=1)
    _log_file.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), msg))


def resolve_shell_tty() -> str | None:
    """Find the board's shell tty via sysfs; mknod the /dev node if missing.

    Same discovery contract as fw/scripts/lib/serial-port.sh
    (_serial_find_board_iface "00") — kept in Python so the bridge has no
    subprocess dependencies in its reconnect loop.
    """
    base = "/sys/class/tty"
    try:
        entries = sorted(e for e in os.listdir(base) if e.startswith("ttyACM"))
    except FileNotFoundError:
        return None
    for name in entries:
        d = os.path.join(base, name)
        try:
            ifdir = os.path.realpath(os.path.join(d, "device"))
            usbdev = os.path.realpath(os.path.join(ifdir, ".."))
            with open(os.path.join(usbdev, "idVendor")) as f:
                vid = f.read().strip()
            with open(os.path.join(usbdev, "idProduct")) as f:
                pid = f.read().strip()
            with open(os.path.join(ifdir, "bInterfaceNumber")) as f:
                ifnum = f.read().strip()
        except OSError:
            continue  # stale sysfs entry mid-re-enumeration
        if vid != BOARD_VID or pid != BOARD_PID or ifnum != SHELL_IFACE:
            continue
        node = "/dev/" + name
        if not os.path.exists(node):
            try:
                with open(os.path.join(d, "dev")) as f:
                    major, minor = (int(x) for x in f.read().strip().split(":"))
                os.mknod(node, stat.S_IFCHR | 0o666, os.makedev(major, minor))
                log("mknod %s (%d:%d)" % (node, major, minor))
            except OSError as e:
                log("mknod %s failed: %s" % (node, e))
                continue
        return node
    return None


def write_all(fd: int, data: bytes) -> None:
    """Write fully, tolerating short writes and EAGAIN.

    The harness spawns this bridge with stdin/stdout/stderr all on ONE open
    file description (the PTY master), and main() sets O_NONBLOCK on fd 0 —
    which, living on the description, makes fd 1 non-blocking too. During
    the boot log flood a bare os.write(1, ...) can short-write (silently
    truncating device output) or raise BlockingIOError (killing the bridge
    and every remaining test). Loop + select-on-writable covers both
    (PR #341 review).
    """
    view = memoryview(data)
    while view:
        try:
            n = os.write(fd, view)
            view = view[n:]
        except BlockingIOError:
            select.select([], [fd], [], 1.0)


def open_raw(node: str) -> int:
    fd = os.open(node, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    # cfmakeraw equivalent: no echo/canonical/signals/translation.
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
    attrs[3] = 0  # lflag
    attrs[4] = BAUD
    attrs[5] = BAUD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main() -> int:
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    # stdin non-blocking so a quiet harness never stalls the device reader.
    fcntl.fcntl(0, fcntl.F_SETFL, fcntl.fcntl(0, fcntl.F_GETFL) | os.O_NONBLOCK)

    dev_fd = -1
    pending = b""  # harness bytes queued while the device is away
    log("bridge started")

    while True:
        if dev_fd < 0:
            node = resolve_shell_tty()
            if node is None:
                time.sleep(RESOLVE_RETRY_S)
                continue
            try:
                dev_fd = open_raw(node)
            except OSError as e:
                log("open %s failed: %s" % (node, e))
                time.sleep(RESOLVE_RETRY_S)
                continue
            log("connected to %s" % node)
            if pending:
                try:
                    write_all(dev_fd, pending)
                except OSError:
                    pass
                pending = b""

        rlist = [0, dev_fd]
        try:
            ready, _, _ = select.select(rlist, [], [], 1.0)
        except InterruptedError:
            continue

        if 0 in ready:
            try:
                data = os.read(0, 4096)
            except (BlockingIOError, InterruptedError):
                data = None
            except OSError:
                data = b""
            if data == b"":
                log("stdin EOF — exiting")
                return 0
            if data:
                try:
                    write_all(dev_fd, data)
                except OSError:
                    # Device just vanished; keep the bytes for the reconnect.
                    pending += data

        if dev_fd in ready:
            try:
                data = os.read(dev_fd, 4096)
            except (BlockingIOError, InterruptedError):
                continue
            except OSError as e:
                if e.errno in (errno.EIO, errno.ENODEV, errno.ENXIO):
                    log("device lost (%s) — re-resolving" % e)
                    os.close(dev_fd)
                    dev_fd = -1
                    continue
                raise
            if data == b"":
                # EOF: CDC gone (flash/reboot window). Re-resolve, never exit.
                log("device EOF — re-resolving")
                os.close(dev_fd)
                dev_fd = -1
                continue
            write_all(1, data)  # unbuffered; stdout IS the device stream


if __name__ == "__main__":
    sys.exit(main())
