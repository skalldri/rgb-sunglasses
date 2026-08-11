"""USB CDC port discovery for the dev board, host side.

Same discovery contract as fw/scripts/lib/serial-port.sh and
fw/scripts/tty-bridge.py: ports are found by USB identity via sysfs — never by
/dev node name, which shifts on every reflash/reboot — and missing /dev nodes
are recreated from sysfs major:minor (the devcontainer has no udev).

The board (VID 2fe3, PID 0001) exposes the Zephyr shell on CDC function 0
(Linux control interface "00") and the MCUmgr/SMP transport on CDC function 1
(interface "02"). The shell port belongs to tty-bridge.py; tests only ever
need the SMP port (for the overridden `mcumgr` fixture).
"""

from __future__ import annotations

import os
import stat

BOARD_VID = "2fe3"
BOARD_PID = "0001"
SHELL_IFACE = "00"
SMP_IFACE = "02"


def _recreate_dev_node(sys_tty_dir: str, node: str) -> bool:
    try:
        with open(os.path.join(sys_tty_dir, "dev")) as f:
            major, minor = (int(x) for x in f.read().strip().split(":"))
        os.mknod(node, stat.S_IFCHR | 0o666, os.makedev(major, minor))
        return True
    except OSError:
        return False


def find_board_tty(iface: str) -> str | None:
    """Return the /dev/ttyACMn bound to the board's given USB interface."""
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
        if vid != BOARD_VID or pid != BOARD_PID or ifnum != iface:
            continue
        node = "/dev/" + name
        if not os.path.exists(node) and not _recreate_dev_node(d, node):
            continue
        return node
    return None


def find_smp_port() -> str | None:
    return find_board_tty(SMP_IFACE)
