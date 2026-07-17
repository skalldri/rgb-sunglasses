#!/usr/bin/env python3
"""
re-pair.py — fast, hands-off, SAFE BLE re-pairing of an Android phone to the nRF5340
dev board. Forgets the stale bond and re-pairs, with a local autoresponder that answers
Android's pairing dialog (types the board's random passkey) fast enough to beat the
connect timeout — no LLM/human in the loop for the time-critical step.

SAFETY (why this is Python, not Bash): a phone can have dozens of bonded devices, and
several boards share the "RGB Sunglasses Proto0" name prefix. Forgetting the WRONG
device would unpair real personal hardware (earbuds, a car, ...). So the forget path:
  - matches the target by its EXACT full name (never a prefix/substring),
  - taps that row's OWN per-device gear (identified by the device name),
  - and taps "Forget" ONLY after verifying the opened details page's header shows that
    exact name — otherwise it backs out and falls back to asking the human.
The device-name verification gate is the hard guarantee that we never forget a device
we haven't positively identified.

LOCKS: touches the board (serial) and the phone (adb) -> requires this session to hold
BOTH the `board` and `app` hw-locks (checked, never acquired). SERIAL: opens and OWNS
the shell UART; close any mcp__serial__ connection first (preflight aborts if it's busy).
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

import xml.etree.ElementTree as ET

try:
    import serial  # pyserial
except ImportError:
    serial = None

PKG = "com.autom8ed.rgbsunglassesapp.dev"
BOARD_VID, BOARD_PID, SHELL_IFACE = "2fe3", "0001", "00"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HW_LOCK = os.path.join(REPO_ROOT, "scripts", "hw-lock.sh")

BOUNDS_RE = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")
PASSKEY_RE = re.compile(rb"Passkey for.*?: (\d{6})")


def log(m):
    print(f"[re-pair] {m}", flush=True)


def warn(m):
    print(f"[re-pair] WARN: {m}", file=sys.stderr, flush=True)


def die(m):
    print(f"[re-pair] ERROR: {m}", file=sys.stderr, flush=True)
    sys.exit(1)


# --------------------------------------------------------------------------- adb
class Adb:
    def __init__(self, serial_id=None):
        self.base = ["adb"] + (["-s", serial_id] if serial_id else [])

    def shell(self, *args, timeout=20):
        return subprocess.run(self.base + ["shell", *args], capture_output=True,
                              text=True, timeout=timeout)

    def out(self, *args, timeout=20):
        return subprocess.run(self.base + list(args), capture_output=True,
                              text=True, timeout=timeout)

    def tap(self, x, y):
        self.shell("input", "tap", str(x), str(y))

    def key(self, *codes):
        self.shell("input", "keyevent", *[str(c) for c in codes])

    def text(self, s):
        self.shell("input", "text", s)

    def state(self):
        return self.out("get-state").stdout.strip()

    def screen_h(self):
        m = re.search(r":\s*\d+x(\d+)", self.out("shell", "wm", "size").stdout)
        return int(m.group(1)) if m else 2400

    def dumpsys_bonded(self):
        # The full "Bonded devices" section (names only; MACs are masked by Android).
        r = self.shell("dumpsys", "bluetooth_manager", timeout=25)
        out, keep, buf = r.stdout, False, []
        for line in out.splitlines():
            if "Bonded devices" in line:
                keep = True
                continue
            if keep:
                if line.strip() == "":
                    break
                buf.append(line)
        return "\n".join(buf) if buf else out  # fall back to whole dump for name search


# ------------------------------------------------------------------ ui automation
class Node:
    __slots__ = ("text", "desc", "rid", "cls", "clickable", "b", "cx", "cy")

    def __init__(self, e):
        self.text = e.get("text", "") or ""
        self.desc = e.get("content-desc", "") or ""
        self.rid = e.get("resource-id", "") or ""
        self.cls = e.get("class", "") or ""
        self.clickable = e.get("clickable") == "true"
        m = BOUNDS_RE.match(e.get("bounds", "") or "")
        if m:
            x1, y1, x2, y2 = map(int, m.groups())
            self.b = (x1, y1, x2, y2)
            self.cx, self.cy = (x1 + x2) // 2, (y1 + y2) // 2
        else:
            self.b = None
            self.cx = self.cy = None


class UI:
    def __init__(self, adb, screen_h):
        self.adb = adb
        self.screen_h = screen_h
        self.nodes = []

    def dump(self):
        try:
            self.adb.shell("uiautomator", "dump", "/sdcard/ui.xml", timeout=15)
            xml = self.adb.out("exec-out", "cat", "/sdcard/ui.xml", timeout=15).stdout
            root = ET.fromstring(xml)
        except Exception:
            self.nodes = []
            return False
        self.nodes = [Node(e) for e in root.iter("node")]
        return bool(self.nodes)

    def has(self, pattern):
        rx = re.compile(pattern, re.I)
        return any(rx.search(" ".join((n.text, n.desc, n.cls, n.rid))) for n in self.nodes)

    def find(self, pattern, ymin=0.0, ymax=1.0):
        """Match pattern against text and content-desc SEPARATELY (so '^Connect$' hits the
        button, not the 'Connect over Bluetooth' header). Returns a Node or None."""
        rx = re.compile(pattern, re.I)
        for n in self.nodes:
            if n.b is None:
                continue
            if not (rx.search(n.text) or rx.search(n.desc)):
                continue
            if ymin * self.screen_h <= n.cy <= ymax * self.screen_h:
                return n
        return None

    def tap(self, pattern, ymin=0.0, ymax=1.0):
        n = self.find(pattern, ymin, ymax)
        if n:
            self.adb.tap(n.cx, n.cy)
            return True
        return False


# ---------------------------------------------------------------- serial watcher
def port_holders(port):
    """PIDs (other than us) holding `port` open — detects an open mcp__serial__
    connection, screen, or fw-shell.sh. Dependency-free (/proc scan); `fuser` isn't
    guaranteed to be installed in the devcontainer."""
    real = os.path.realpath(port)
    me = str(os.getpid())
    holders = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or pid == me:
            continue
        fddir = f"/proc/{pid}/fd"
        try:
            for fd in os.listdir(fddir):
                try:
                    if os.path.realpath(f"{fddir}/{fd}") == real:
                        holders.append(pid)
                        break
                except OSError:
                    continue
        except OSError:
            continue
    return holders


def find_shell_port():
    """Discover the board's Zephyr shell tty by USB identity (iface 00 of 2fe3:0001),
    recreating any missing /dev nodes (devcontainer has no udev)."""
    base = "/sys/class/tty"
    if not os.path.isdir(base):
        return None
    for name in os.listdir(base):
        if not name.startswith("ttyACM"):
            continue
        dev = f"/dev/{name}"
        if not os.path.exists(dev):
            try:
                maj, minr = open(f"{base}/{name}/dev").read().strip().split(":")
                os.system(f"mknod {dev} c {maj} {minr} && chmod 666 {dev}")
            except Exception:
                continue
    for name in sorted(os.listdir(base)):
        if not name.startswith("ttyACM"):
            continue
        try:
            ifdir = os.path.realpath(f"{base}/{name}/device")
            usb = os.path.realpath(f"{ifdir}/..")
            vid = open(f"{usb}/idVendor").read().strip()
            pid = open(f"{usb}/idProduct").read().strip()
            iface = open(f"{ifdir}/bInterfaceNumber").read().strip()
        except Exception:
            continue
        if vid == BOARD_VID and pid == BOARD_PID and iface == SHELL_IFACE:
            return f"/dev/{name}"
    return None


class SerialWatcher:
    """Owns the shell UART: a background thread accumulates all bytes; the pairing loop
    scans the buffer after a recorded offset so a prior attempt's random passkey can't
    replay. Also used to drive `bt_state` for verification."""

    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.alive = True
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        while self.alive:
            try:
                data = self.ser.read(4096)
            except Exception:
                break
            if data:
                with self.lock:
                    self.buf.extend(data)

    def offset(self):
        with self.lock:
            return len(self.buf)

    def _after(self, off):
        with self.lock:
            return bytes(self.buf[off:])

    def scan_after(self, off, pattern):
        return re.search(pattern.encode() if isinstance(pattern, str) else pattern,
                         self._after(off))

    def passkey_after(self, off):
        found = PASSKEY_RE.findall(self._after(off))
        return found[-1].decode() if found else None

    def write(self, data):
        try:
            self.ser.write(data)
        except Exception:
            pass

    def close(self):
        self.alive = False
        try:
            self.ser.close()
        except Exception:
            pass


# ------------------------------------------------------------------------ device
class RePair:
    def __init__(self, args):
        self.a = Adb(args.serial)
        self.name = args.device_name
        self.attempts = args.attempts
        self.keyevents = args.keyevents
        self.connect_timeout = args.timeout_connect
        self.forget_mode = args.forget_mode
        self.screen_h = None
        self.ui = None
        self.port = None
        self.watch = None

    # ---- preflight ----
    def preflight(self):
        # The board+app hw-locks coordinate the shared board+phone across Claude
        # Code agent worktrees. Only enforce them when an agent is driving --
        # Claude Code sets CLAUDECODE=1 in every command it spawns; a solo human
        # runs lock-free. RGBSG_NO_LOCK=1 forces the lock-free path.
        if os.environ.get("CLAUDECODE") and not os.environ.get("RGBSG_NO_LOCK"):
            for res in ("board", "app"):
                if os.access(HW_LOCK, os.X_OK):
                    if subprocess.run([HW_LOCK, "check", res], capture_output=True).returncode != 0:
                        die(f"the '{res}' hw-lock is not held by this session. Hold both:\n"
                            f'    Monitor(command: "scripts/hw-lock.sh hold board app", persistent: true)')
        if self.a.state() != "device":
            die("no adb device (get-state != 'device'). Check `adb devices`.")
        if serial is None:
            die("pyserial not available (import serial failed).")
        self.screen_h = self.a.screen_h()
        self.ui = UI(self.a, self.screen_h)
        self.port = find_shell_port()
        if not self.port:
            die("no Zephyr shell port (board 2fe3:0001 not found).")
        # port ownership: refuse if another process (an open MCP serial conn) holds it
        holders = port_holders(self.port)
        if holders:
            die(f"{self.port} is held by another process (an open mcp__serial__ connection?).\n"
                f"    Close it first (mcp__serial__serial_close). pids: {', '.join(holders)}")
        log(f"shell UART: {self.port}   screen height: {self.screen_h}px   target: '{self.name}'")

    def is_bonded(self):
        # EXACT full-name match: "RGB Sunglasses" alone is ambiguous (multiple boards).
        return self.name in self.a.dumpsys_bonded()

    # ---- forget (SAFE, device-name-verified) ----
    # Unpair/forget action label differs by OEM: AOSP/Pixel says "Forget", OxygenOS says
    # "Unpair". Match either (anchored where we tap so it never hits an unrelated control).
    _UNPAIR_RE = r"\b(forget|unpair)\b"

    def _details_page_for(self, name):
        """True ONLY if the current screen is the details page of EXACTLY `name`. This is
        the safety gate before tapping Unpair - it must never pass on the wrong device."""
        # Primary (AOSP/Pixel): the header element carries the device name.
        for n in self.ui.nodes:
            if n.rid.endswith("bt_header_device_name"):
                return n.text.strip() == name
        # Cross-OEM: the details page shows the device name AND offers an unpair/forget
        # action. A device's details page only ever shows its own name, so name-match +
        # unpair-action = we're on THIS device's page. The match is CONTAINS, not
        # equality: Android 16's Pixel page has only a generic "Device details" header
        # and embeds the name mid-sentence in the can't-connect banner ("Try restarting
        # RGB Sunglasses Proto0 94E0. If that doesn't ..."). Contains stays safe as the
        # gate: names carry the unique per-board serial suffix, so a different device's
        # page can never contain the target's exact full name.
        has_name = any(name in (n.text + " " + n.desc) for n in self.ui.nodes)
        return has_name and self.ui.has(self._UNPAIR_RE)

    def _is_some_details_page(self):
        # A device details page (either OEM) offers an unpair/forget action; the device
        # LIST does not. Used to back out of a deep-linked details page to reach the list.
        return self.ui.has(self._UNPAIR_RE)

    # The per-device settings gear is identified by rid/desc so we tap IT, never the
    # row body (which on OxygenOS toggles connect). Pixel: rid=settings_button, desc
    # "<name>. Configure device detail."; OxygenOS: rid=deviceDetails, desc "Device Settings".
    _GEAR_RE = re.compile(r"settings_button|deviceDetails|configure device detail|device settings", re.I)

    def _find_device_gear(self, name):
        """Return the (x,y) of the per-device settings gear for EXACTLY `name`, or None."""
        # Fast path (Pixel): the gear's own desc carries the device name - unambiguous.
        for n in self.ui.nodes:
            if n.clickable and n.b and n.desc.startswith(name + ".") \
                    and "configure device detail" in n.desc.lower():
                return (n.cx, n.cy)
        # General: find the target's row by its exact title text, then the gear IN THAT ROW
        # (matched by rid/desc, so we never grab the row's connect target). The gear sits at
        # the row's right edge, so take the rightmost matching node.
        title = next((n for n in self.ui.nodes if n.text.strip() == name and n.b), None)
        if not title:
            return None
        y1, y2 = title.b[1], title.b[3]
        gears = [n for n in self.ui.nodes
                 if n.clickable and n.b and (y1 - 10) <= n.cy <= (y2 + 10)
                 and self._GEAR_RE.search(n.rid + " " + n.desc)]
        if gears:
            g = max(gears, key=lambda n: n.cx)
            return (g.cx, g.cy)
        return None

    def _scroll_down(self):
        # Drag SLOWLY (800ms): a sub-500ms swipe is silently dropped on some
        # devices/pages - observed on a Pixel 9 Pro (Android 16), where 300ms
        # flings scrolled the Saved-devices list not one pixel while an 800ms
        # drag worked every time. Screenshot-diff verified, not dump-inferred.
        w = 500
        self.a.shell("input", "swipe", str(w), str(int(self.screen_h * 0.85)),
                     str(w), str(int(self.screen_h * 0.20)), "800")
        time.sleep(0.8)

    def _manual_forget(self):
        print("", flush=True)
        print(f">>> Please FORGET '{self.name}' in the phone's Bluetooth settings now.", flush=True)
        print(">>> (Settings > Bluetooth > that device's gear > Forget.)  Waiting up to 120s…", flush=True)
        waited = 0
        while self.is_bonded():
            time.sleep(2)
            waited += 2
            if waited >= 120:
                die("bond still present after 120s — aborting.")
        log("forget: bond cleared (manual).")

    def forget(self):
        if self.forget_mode == "none":
            log("forget: skipped (--no-forget).")
            return
        if not self.is_bonded():
            log(f"forget: '{self.name}' not in the phone's bonded list — nothing to forget.")
            return
        if self.forget_mode == "manual":
            return self._manual_forget()

        log(f"forget: locating '{self.name}' in Bluetooth settings…")
        self.a.shell("am", "start", "-a", "android.settings.BLUETOOTH_SETTINGS")
        time.sleep(1.5)
        # Get to the device LIST: `am start` can land on a details page — back out of it.
        for _ in range(3):
            self.ui.dump()
            if self._is_some_details_page():
                self.a.key(4)  # BACK
                time.sleep(1.0)
            else:
                break
        # Find the target's own gear. On stock Android the first Bluetooth screen only
        # lists connected + a few saved devices; the rest (incl. a not-connected board)
        # live behind "See all" -> the full "Saved devices" list. The "See all" row
        # itself can start entirely OFF-SCREEN (Android 16 appends it after the
        # recent-saved rows), so hunt for it BY scrolling — the old "break when not on
        # the first dump" bailed out here and then scrolled straight past it to the
        # page bottom without ever expanding the list (observed on a Pixel 9 Pro /
        # Android 16 with 40+ bonds). When visible but clipped into the gesture-nav
        # strip, scroll once more before tapping (a tap in the strip misses).
        self.ui.dump()
        gear = self._find_device_gear(self.name)
        if not gear:
            for _ in range(8):
                n = self.ui.find(r"^See all$")
                if n and n.cy <= self.screen_h - 220:
                    log("expanding the full device list (See all)…")
                    self.a.tap(n.cx, n.cy)
                    time.sleep(1.2)
                    self.ui.dump()
                    break
                self._scroll_down()
                self.ui.dump()
                # The target's gear can also come into view directly while hunting
                # (OxygenOS lists every bond inline and has no "See all" at all).
                gear = self._find_device_gear(self.name)
                if gear:
                    break
        if not gear:
            for _ in range(14):
                gear = self._find_device_gear(self.name)
                if gear:
                    break
                self._scroll_down()
                self.ui.dump()
        if not gear:
            warn(f"could not locate '{self.name}' in the settings list — falling back to manual.")
            return self._manual_forget()

        self.a.tap(*gear)
        time.sleep(1.2)
        self.ui.dump()
        # SAFETY GATE: only forget if the opened details page is EXACTLY the target.
        if not self._details_page_for(self.name):
            warn("opened details page does NOT match the target device — NOT forgetting. "
                 "Backing out and falling back to manual.")
            self.a.key(4)
            return self._manual_forget()

        log(f"forget: on '{self.name}' details page (verified) — tapping Forget/Unpair.")
        if not self.ui.tap(r"^forget$|^unpair$"):
            warn("no Forget/Unpair action on the details page — manual fallback.")
            return self._manual_forget()
        time.sleep(0.8)
        self.ui.dump()
        self.ui.tap(r"^forget( device)?$|^unpair$|^ok$")  # confirmation dialog, if any
        time.sleep(1.2)
        if self.is_bonded():
            warn("bond still present after automated forget — manual fallback.")
            return self._manual_forget()
        log("forget: bond cleared.")

    # ---- pairing ----
    def relaunch_app(self):
        log("relaunching app (clears any orphaned native BLE link)…")
        self.a.shell("am", "force-stop", PKG)
        time.sleep(1)
        self.a.shell("monkey", "-p", PKG, "-c", "android.intent.category.LAUNCHER", "1")
        time.sleep(4)

    def _dismiss_update_modal(self):
        # Modal identified by its 'Current: v…/Latest: v…' body — NOT the persistent
        # 'App update available:' banner (a blind BACK on the banner exits the app).
        if self.ui.has(r"Current: v|Latest: v"):
            log("dismissing app-update modal.")
            self.a.key(4)
            time.sleep(1)
            return True
        return False

    def tap_connect(self):
        deadline = time.time() + 25
        while time.time() < deadline:
            if not self.ui.dump():
                time.sleep(1)
                continue
            if self._dismiss_update_modal():
                continue
            if self.ui.has(re.escape(self.name)):
                if self.ui.tap(r"^Connect$", 0.0, 0.88):
                    log("tapped Connect.")
                    return True
            time.sleep(1.5)
        warn(f"board '{self.name}' not listed / Connect not tappable within 25s.")
        return False

    def _editext_center(self):
        for n in self.ui.nodes:
            if n.cls == "android.widget.EditText" and n.b:
                return (n.cx, n.cy)
        return None

    def enter_passkey(self, pk):
        xy = self._editext_center()
        if not xy:
            return False
        self.a.tap(*xy)          # focus
        time.sleep(0.3)
        self.a.key(123, 67, 67, 67, 67, 67, 67)  # MOVE_END + DEL x6 (clear residue)
        if self.keyevents:
            for d in pk:
                self.a.key(f"KEYCODE_{d}")
        else:
            self.a.text(pk)
        log(f"typed passkey {pk} into PIN field.")
        time.sleep(0.3)
        self.a.key(66)           # ENTER (IME checkmark)
        time.sleep(0.3)
        self.ui.dump()
        self.ui.tap(r"^pair$|^ok$|^save$|^done$|^confirm$")  # dialog Save/OK, if present
        return True

    def answer_pairing(self, off):
        deadline = time.time() + self.connect_timeout
        pk = None
        while time.time() < deadline:
            if pk is None:
                pk = self.watch.passkey_after(off)
            if self.watch.scan_after(off, r"Pairing completed"):
                log("pairing completed.")
                return True
            if self.watch.scan_after(off, r"Pairing failed|Pairing cancelled|Disconnected \(reason 19\)"):
                warn("pairing failed/cancelled (UART).")
                return False
            if not self.ui.dump():
                time.sleep(0.5)
                continue
            # 1. PIN-entry dialog present -> type the board's passkey once it's known.
            if self.ui.has(r'android\.widget\.EditText'):
                if pk is None:          # wait for the board to print the passkey
                    time.sleep(0.4)
                    continue
                self.enter_passkey(pk)
                time.sleep(1.0)
                continue
            # 2. On-screen confirm dialog (OxygenOS shows a persistent
            #    'Bluetooth pairing request' + Pair button).
            if self.ui.tap(r"^pair$|^ok$", 0.0, 1.0):
                log("tapped Pair (confirm).")
                time.sleep(0.8)
                continue
            # 3. Stock Android (Pixel): the prompt is a TRANSIENT heads-up notification
            #    that vanishes fast and drops into the shade. Tap it (as a heads-up banner
            #    or in the pulled-down shade) to open the real pairing dialog, which the
            #    steps above then handle next iteration.
            if self.ui.tap(r"tap to pair|pair with|pairing request|pair & connect"):
                log("tapped pairing notification.")
                self.a.shell("cmd", "statusbar", "collapse")
                time.sleep(0.8)
                continue
            # 4. Not visible yet -> pull the notification shade down to surface it.
            self.a.shell("cmd", "statusbar", "expand-notifications")
            time.sleep(0.6)
        warn(f"deadline ({self.connect_timeout}s) reached without 'Pairing completed'.")
        return False

    def pair_once(self):
        self.relaunch_app()
        off = self.watch.offset()
        if not self.tap_connect():
            return False
        return self.answer_pairing(off)

    # ---- verify ----
    def verify(self):
        log("verifying via bt_state…")
        deadline = time.time() + 60
        while time.time() < deadline:
            off = self.watch.offset()
            self.watch.write(b"bt_state\r")
            time.sleep(2)
            snap = self.watch._after(off)
            if b"ATT MTU: 23" in snap:
                warn("bt_state shows ATT MTU: 23 — split-brain persists (hard fail).")
                return False
            if b"CONNECTED" in snap and b"Security level: L4" in snap:
                m = re.search(rb"ATT MTU: (\d+)", snap)
                if m and int(m.group(1)) > 23:
                    log(f"verified: CONNECTED, L4, ATT MTU: {int(m.group(1))}.")
                    return True
        warn("bt_state did not reach a healthy CONNECTED/L4/MTU>23 within 60s.")
        return False

    # ---- run ----
    def run(self):
        self.preflight()
        self.forget()
        if self.forget_mode == "only":
            log("done (--forget-only).")
            return 0
        self.watch = SerialWatcher(self.port)
        try:
            ok = False
            for i in range(1, self.attempts + 1):
                log(f"=== pairing attempt {i}/{self.attempts} ===")
                if self.pair_once():
                    ok = True
                    break
                warn(f"attempt {i} failed; retrying…")
                time.sleep(1)
            if not ok:
                die(f"pairing did not complete after {self.attempts} attempts.")
            if self.verify():
                log("SUCCESS — re-paired and verified (CONNECTED / L4 / MTU>23).")
                return 0
            die("paired, but on-device verification failed (see bt_state above).")
        finally:
            self.watch.close()
            self.a.shell("cmd", "statusbar", "collapse")


def main():
    p = argparse.ArgumentParser(description="Fast, safe automated BLE re-pairing.")
    p.add_argument("--serial", help="adb serial (default: sole connected device)")
    p.add_argument("--device-name", default="RGB Sunglasses Proto0 8996",
                   help="EXACT full advertised name of the target board (never a prefix)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--no-forget", dest="forget_mode", action="store_const", const="none")
    g.add_argument("--forget-only", dest="forget_mode", action="store_const", const="only")
    g.add_argument("--manual-forget", dest="forget_mode", action="store_const", const="manual")
    p.set_defaults(forget_mode="auto")
    p.add_argument("--attempts", type=int, default=3)
    p.add_argument("--keyevents", action="store_true",
                   help="type the passkey as per-digit key events instead of input text")
    p.add_argument("--timeout-connect", type=int, default=60,
                   help="per-attempt pairing-dialog deadline (matches the app connect timeout)")
    args = p.parse_args()
    sys.exit(RePair(args).run())


if __name__ == "__main__":
    main()
