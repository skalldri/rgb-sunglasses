"""RgbShell — the suite's one way to talk to the board's Zephyr shell.

Wraps pytest-twister-harness's Shell/DeviceAdapter with the quirks this board
is known to need (catalogued in fw/CLAUDE.md and .serial_mcp/plugins/
rgb_sunglasses.py, the interactive-session equivalent of this class):

- Ctrl+C before every command: a boot-log fragment can land in the shell's
  line editor before the first command and corrupt it; Ctrl+C is cheap and
  fully general.
- `retval` after every command: CONFIG_SHELL_CMDS_RETURN_VALUE prints the last
  command's return value as a bare integer — the machine-readable pass/fail
  primitive. Tests assert exit codes, not prose.
- reboot() survives USB re-enumeration because the harness talks to
  tty-bridge.py's PTY, which re-resolves the CDC port underneath.
- The TPS25750 driver logs ~10 ms after boot and the shell redraws its prompt
  after every async log line, so the first exchange after a (re)connect can be
  eaten — sync() retries a cheap command until the link is clean.
"""

from __future__ import annotations

import logging
import re
import time

from twister_harness import DeviceAdapter, Shell
from twister_harness.exceptions import TwisterHarnessTimeoutException

logger = logging.getLogger(__name__)

PROMPT_RE = r"uart:~\$"

# The settings store coalesces writes and flushes CONFIG_APP_SETTINGS_SAVE_
# DEBOUNCE_MS (1000 ms) after the last one; 3 s is a generous margin. Single
# source of truth for the persist-flush wait, so a debounce-config change is a
# one-line update instead of chasing hardcoded sleeps across the suite (#362).
PERSIST_FLUSH_S = 3.0

# "0x20001234 thread_name (real size 2048):  unused 1234  usage 814 / 2048 (39 %)"
# The percentage is printed with %2u ("( 7 %)" below 10%) — the \s* after the
# open paren is load-bearing; without it every thread under 10% was silently
# dropped from the dict (PR #341 review).
_STACKS_RE = re.compile(
    r"^0x[0-9a-fA-F]+\s+(?P<name>\S+(?: \d+)?)\s*"
    r"\(real size\s+(?P<size>\d+)\):\s+unused\s+\d+\s+"
    r"usage\s+(?P<used>\d+)\s+/\s+\d+\s+\(\s*(?P<pct>\d+)\s*%\)"
)
# "kernel thread list": " 0x20001234 thread_name" header then
# "  options: 0x0, priority: 14 timeout: 0"
_THREAD_HDR_RE = re.compile(r"^\*?\s*0x[0-9a-fA-F]+\s+(?P<name>\S+)")
_THREAD_PRIO_RE = re.compile(r"priority:\s+(?P<prio>-?\d+)")
_KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_.]*)=(-?\d+)")
_INT_RE = re.compile(r"^-?\d+$")


class ShellCommandFailed(AssertionError):
    pass


class RgbShell:
    def __init__(self, dut: DeviceAdapter, shell: Shell):
        self.dut = dut
        self.shell = shell

    # ---- plumbing --------------------------------------------------------

    def wait_persist_flush(self) -> None:
        """Block long enough for a debounced settings write to reach NVS
        (PERSIST_FLUSH_S). Use after any BT/shell config write whose value a
        later reboot must see."""
        time.sleep(PERSIST_FLUSH_S)

    def sync(self, timeout: float = 30.0) -> None:
        """Get the shell to a clean, responsive prompt (post-boot/reconnect)."""
        deadline = time.time() + timeout
        last_exc: Exception | None = None
        while time.time() < deadline:
            try:
                self.dut.write(b"\x03")  # cancel whatever sits in the line editor
                time.sleep(0.2)
                self.dut.clear_buffer()
                out = self.shell.exec_command("kernel uptime", timeout=5.0)
                if any("Uptime:" in line or "uptime" in line.lower() for line in out):
                    return
            except Exception as exc:  # timeout mid-boot: retry until deadline
                last_exc = exc
            time.sleep(0.5)
        raise TimeoutError(f"shell did not become responsive: {last_exc}")

    def wait_for_quiet(self, window: float = 1.0, max_wait: float = 30.0) -> None:
        """Wait until the console has been silent for `window` seconds.

        Boot floods the console (extension discovery, BT/USB bring-up) for
        several seconds after the prompt first appears; a command's echo can
        get smeared across those log lines and never match (hardware-observed
        on the first suite run). Fixtures call this once before starting.
        """
        deadline = time.time() + max_wait
        while time.time() < deadline:
            try:
                self.dut.readline(timeout=window, print_output=False)
                # A line arrived — console still noisy; keep waiting.
            except TwisterHarnessTimeoutException:
                return  # a full quiet window elapsed
        logger.warning("console never went quiet for %.1fs; proceeding", window)

    def wait_boot_settled(self, timeout: float = 45.0) -> None:
        """Wait until application boot init has fully completed.

        `pattern_controller_thread`'s boot sequence ends with the switch to
        the default animation, strictly AFTER extension discovery and
        registration (see pattern_controller.cpp) — so `anim get` != "none"
        is an exact "boot is done" barrier. Without it, a test that runs
        right after the prompt appears sees an empty `ext list` and
        `anim get` == none (hardware-observed).
        """
        self.wait_for_quiet(window=1.0, max_wait=20.0)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if self.anim_get() != "none":
                    return
            except Exception:
                pass
            time.sleep(1.0)
        logger.warning("boot settle: `anim get` still 'none' after %.0fs", timeout)

    def _exec_with_retry(self, cmd: str, timeout: float | None, attempts: int = 3) -> list[str]:
        """One shell exchange, retried on echo-smear timeouts.

        An async log line can interleave with the command's echo so the
        harness never matches it (the same failure mode the serial-MCP
        plugin's _run_command retries around). Flush + resend converges on
        the second try in practice.
        """
        last_exc: Exception | None = None
        for attempt in range(attempts):
            try:
                return self.shell.exec_command(cmd, timeout=timeout or 10.0)
            except TwisterHarnessTimeoutException as exc:
                last_exc = exc
                logger.warning("exchange %r timed out (attempt %d); flush + retry", cmd, attempt + 1)
                self.dut.write(b"\x03")
                time.sleep(0.2)
                self.dut.clear_buffer()
        raise AssertionError(f"exchange {cmd!r} failed after {attempts} attempt(s): {last_exc}")

    def exec(
        self,
        cmd: str,
        timeout: float | None = None,
        check: bool = True,
        attempts: int = 3,
    ) -> list[str]:
        """Run a command; return its filtered output lines.

        With check=True (default), also runs `retval` and fails the test if
        the command's return value was non-zero.

        IDEMPOTENT COMMANDS ONLY: an echo-smear timeout re-sends the command
        (see _exec_with_retry), so a command that already executed can run
        twice. Reads, absolute-value sets, and re-runnable commands (`fatfs
        corrupt confirm` is deliberately safe to repeat) are fine. A command
        that reboots the device out from under the exchange (`crash panic`,
        `factory_reset now|soft`) must use exec_oneway() + wait_reboot()
        instead; a non-idempotent one-shot (e.g. a delete) should pass
        check=False and verify its effect separately.
        """
        self.dut.write(b"\x03")
        time.sleep(0.05)
        self.dut.clear_buffer()
        # attempts=1 makes the send single-shot for commands whose EFFECT is
        # not idempotent even though re-running them is syntactically fine
        # (e.g. re-arming a fault injector after the fault already cleared
        # it): an echo smear then fails the exchange instead of double-firing.
        out = self.shell.get_filtered_output(self._exec_with_retry(cmd, timeout, attempts))
        if check:
            rv = self.last_retval()
            if rv != 0:
                raise ShellCommandFailed(
                    f"`{cmd}` returned {rv}; output: {out!r}"
                )
        return out

    def last_retval(self) -> int:
        # Known blind spot: `retval` itself returns 0 and replaces the stored
        # value (shell_cmds.c cmd_get_retval), so if a `retval` exchange
        # executes but its echo smears and we retry, the retry reads 0. The
        # window is a double rarity (echo-smear on retval AND a failing
        # command); tests assert parsed values, not just exit codes, so a
        # masked non-zero can't silently pass a test on its own.
        lines = self.shell.get_filtered_output(self._exec_with_retry("retval", None))
        for line in lines:
            token = line.strip()
            if _INT_RE.match(token):
                return int(token)
        raise AssertionError(f"could not parse `retval` output: {lines!r}")

    def probe(self, cmd: str, pattern: str, timeout: float = 3.0) -> re.Match | None:
        """One cheap raw exchange: write cmd, return the first pattern match.

        No retries, no retval, no prompt discipline — for windows where the
        full exec() path is either too slow (its retry backoff can burn 30 s
        against the boot flood) or semantically wrong (probing whether the
        board is even alive). Returns None on any failure.
        """
        try:
            self.dut.write(b"\x03")
            time.sleep(0.1)
            self.dut.clear_buffer()
            self.dut.write((cmd + "\n").encode())
            lines = self.dut.readlines_until(
                regex=pattern, timeout=timeout, print_output=False
            )
        except Exception:
            return None
        for line in reversed(lines):
            m = re.search(pattern, line)
            if m:
                return m
        return None

    def probe_uptime_ms(self) -> int | None:
        m = self.probe("kernel uptime", r"Uptime:\s*(\d+)\s*ms")
        return int(m.group(1)) if m else None

    def mark_reboot_reference(self) -> None:
        """Snapshot the current uptime as wait_reboot()'s freshness
        reference, for reboots triggered OUTSIDE the shell (mcumgr reset,
        J-Link) where exec_oneway()'s automatic snapshot never runs."""
        self._oneway_ref_uptime = self.probe_uptime_ms()

    def exec_oneway(self, cmd: str) -> None:
        """Fire a command that will NOT come back to a usable prompt.

        For one-shot/destructive commands (`crash panic`, `factory_reset
        now|soft`) where exec()'s echo-wait would hang and its retry would
        double-fire. Raw write, no echo match, no retval — the caller owns
        re-acquiring the shell (wait_reboot()).

        Snapshots the CURRENT uptime first so wait_reboot() can demand a
        reading strictly LOWER than it — an absolute "uptime < 20 s"
        threshold matched the pre-reboot shell whenever the prior boot was
        itself young (PR #346 review: reachable in the destructive tier,
        where factory_reset fires ~15-20 s into a fresh boot).
        """
        self._oneway_ref_uptime = self.probe_uptime_ms()
        logger.info("one-way command: %s (ref uptime %s ms)", cmd, self._oneway_ref_uptime)
        self.dut.write(b"\x03")
        time.sleep(0.1)
        self.dut.clear_buffer()
        self.dut.write((cmd + "\n").encode())

    def wait_reboot(self, timeout: float = 120.0) -> None:
        """Re-acquire the shell after a reboot this host did not command
        (crash recovery, factory_reset's own reboot).

        Waits for PROOF of a fresh boot, not merely a prompt: `factory_reset
        soft` spends seconds erasing NVS before its reboot, and a prompt-only
        wait matched the STILL-ALIVE pre-reboot shell (hardware-observed:
        `ext list` was then read mid-rescan on the real boot and came back
        partial). Proof = an uptime reading strictly LOWER than the one
        exec_oneway() snapshotted before firing (falling back to a <20 s
        absolute bound when no snapshot exists) — the absolute bound alone
        matched a pre-reboot shell whose boot was itself young.
        """
        ref = getattr(self, "_oneway_ref_uptime", None)
        self._oneway_ref_uptime = None
        deadline = time.time() + timeout
        fresh = False
        while time.time() < deadline:
            up = self.probe_uptime_ms()  # cheap raw probe, never exec()
            if up is not None and (up < ref if ref is not None else up < 20_000):
                fresh = True
                break
            time.sleep(1.0)
        if not fresh:
            raise TimeoutError(f"no fresh boot observed within {timeout}s (ref={ref})")
        self.wait_boot_settled()

    def reboot(self, cold: bool = False, timeout: float = 90.0, settle: bool = True) -> list[str]:
        """Reboot the board and wait for the shell to come back.

        Returns whatever console output happened to be captured while
        waiting — DIAGNOSTICS ONLY, never assert on it. It is structurally
        incomplete twice over: the USB CDC console only enumerates ~8 s into
        boot (everything earlier — settings_load, registry population — is
        gone before the host can listen), and any readlines window that ends
        without a prompt raises and contributes nothing to the capture
        (PR #341 review: an assertion on this return value could never fail).
        """
        cmd = "kernel reboot cold" if cold else "kernel reboot warm"
        logger.info("rebooting board: %s", cmd)
        self.dut.clear_buffer()
        self.dut.write((cmd + "\n").encode())
        time.sleep(3.0)  # let the board actually drop off the bus
        captured: list[str] = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                captured += self.dut.readlines_until(
                    regex=PROMPT_RE, timeout=5.0, print_output=False
                )
                break
            except Exception:
                # Prompt not seen yet — poke the (possibly reattached) shell.
                try:
                    self.dut.write(b"\n")
                except Exception:
                    pass
        self.sync(timeout=max(5.0, deadline - time.time()))
        # settle=False returns at the prompt, BEFORE boot init completes —
        # for tests that must race the boot itself (the #225 advertising
        # gate). Everything else wants the settled barrier.
        if settle:
            self.wait_boot_settled()
        return captured

    # ---- parsers ---------------------------------------------------------

    @staticmethod
    def parse_kv(lines: list[str]) -> dict[str, int]:
        """Collect every KEY=<int> pair across the given lines.

        Matches the house `shell_print` style used by `power bq status`,
        `power bq limits`, `power policy`, `power pd contract`, `bt_state`.
        Later duplicates win (power bq limits prints CHG_STAT once only).
        """
        kv: dict[str, int] = {}
        for line in lines:
            for key, val in _KV_RE.findall(line):
                kv[key] = int(val)
        return kv

    def bq_status(self) -> dict[str, int]:
        return self.parse_kv(self.exec("power bq status"))

    def bq_limits(self) -> dict[str, int]:
        return self.parse_kv(self.exec("power bq limits"))

    def policy(self) -> dict[str, int]:
        return self.parse_kv(self.exec("power policy"))

    def pd_contract(self) -> dict[str, int]:
        out = self.exec("power pd contract")
        kv = self.parse_kv(out)
        for line in out:
            m = re.search(r"available:\s+(\d+)\s+mV\s+@\s+(\d+)\s+mA", line)
            if m:
                kv["available_mv"] = int(m.group(1))
                kv["available_ma"] = int(m.group(2))
        return kv

    def stacks(self) -> dict[str, dict[str, int]]:
        """`kernel thread stacks` → {thread name: {used, size, pct}}."""
        result: dict[str, dict[str, int]] = {}
        for line in self.exec("kernel thread stacks", timeout=30.0):
            m = _STACKS_RE.match(line.strip())
            if m:
                result[m.group("name")] = {
                    "used": int(m.group("used")),
                    "size": int(m.group("size")),
                    "pct": int(m.group("pct")),
                }
        return result

    def thread_priorities(self) -> dict[str, int]:
        """`kernel thread list` → {thread name: priority}."""
        result: dict[str, int] = {}
        current: str | None = None
        for line in self.exec("kernel thread list", timeout=30.0):
            hdr = _THREAD_HDR_RE.match(line.strip())
            if hdr and "priority:" not in line:
                current = hdr.group("name")
                continue
            prio = _THREAD_PRIO_RE.search(line)
            if prio and current is not None:
                result[current] = int(prio.group("prio"))
                current = None
        return result

    def anim_get(self) -> str:
        out = self.exec("anim get")
        assert out, "`anim get` printed nothing"
        return out[-1].strip()

    def glim_list(self) -> list[str]:
        """Indexed .glim names, [] when the directory is empty/missing."""
        names: list[str] = []
        for line in self.exec("glim list"):
            m = re.match(r"^\d+:\s+(\S+)", line.strip())
            if m:
                names.append(m.group(1))
        return names

    def glim_selected(self) -> str | None:
        """The currently selected .glim NAME, or None ("(none)")."""
        for line in self.exec("glim get_selected"):
            m = re.match(r"^\d+:\s+(\S+)", line.strip())
            if m:
                return m.group(1)
        return None

    def glim_select_name(self, name: str) -> None:
        """Select a .glim by NAME (indices are per-boot; names are identity)."""
        names = self.glim_list()
        assert name in names, f"{name!r} not in glim list: {names}"
        self.exec(f"glim select {names.index(name)}")

    def ext_list(self) -> list[dict]:
        """[{slot, id, name, file, params, active, faulted, retired}]"""
        slots: list[dict] = []
        for line in self.exec("ext list"):
            m = re.match(
                r"^\[(\d+)\]\s+id=0x([0-9a-fA-F]+)\s+'(.*)'\s+file=(\S+)\s+params=(\d+)(.*)$",
                line.strip(),
            )
            if not m:
                continue
            flags = m.group(6)
            slots.append(
                {
                    "slot": int(m.group(1)),
                    "id": int(m.group(2), 16),
                    "name": m.group(3),
                    "file": m.group(4),
                    "params": int(m.group(5)),
                    "active": "[active]" in flags,
                    "faulted": "[FAULTED]" in flags,
                    "retired": "[RETIRED" in flags,
                }
            )
        return slots

    def ext_param(self, slot: int, idx: int, name: str | None = None) -> str:
        """Raw value of `ext param <slot> <idx>` — the value after ' = '.

        Output is `<DisplayName>.<ParamName> = <value>` (extension_host.cpp
        cmd_ext_param, lines 1573-1595): `50` for uint32, `0 (0x0)` for bool,
        `0x00ff80` for color (`0x%06x` of a 24-bit-masked value — six hex
        digits, not eight), `"HELLO"` for string. Callers wanting an int use
        ext_param_int.

        `Shell.get_filtered_output` (NCS shell.py) only strips prompt and
        `<dbg>/<inf>/<wrn>/<err>` log lines — an interleaved `printk` or a
        `shell_print` from another subsystem can still reach `out`, and a
        naive "first line with a dot and a later `=`" match would return it.
        The per-test parsers this helper replaced were immune because they
        scanned every line and selected by param name; restore that here:
        collect every `<label> = <value>` line, and

          * if `name` is given, keep only the line whose ParamName matches it.
            This is the guard that ties a hardcoded index to the param it is
            meant to read (the old `k.lower().endswith("speed")` check) — a
            manifest reorder then fails the READ loudly instead of silently
            returning, and letting the caller write, the wrong param. That
            matters most for hello, whose params 2/3 are the Crash/Hang fault
            injectors (PR #365 review).
          * otherwise require exactly one match, so an interleaved line that
            happens to look like `k = v` can't silently win.
        """
        out = self.exec(f"ext param {slot} {idx}")
        matches: list[tuple[str, str]] = []  # (param_name, value)
        for line in out:
            m = re.match(r"^\s*(\S.*?)\s*=\s*(.*\S)\s*$", line)
            if not m:
                continue
            label, value = m.group(1), m.group(2)
            param_name = label.rsplit(".", 1)[-1]  # <display>.<param>
            matches.append((param_name, value))
        if name is not None:
            matches = [(p, v) for (p, v) in matches if p.lower() == name.lower()]
            assert len(matches) == 1, (
                f"expected exactly one `ext param {slot} {idx}` line for param "
                f"{name!r}, got {matches or 'none'} (manifest reorder? #365): {out}"
            )
        else:
            assert len(matches) == 1, (
                f"expected exactly one `<name> = <value>` line from "
                f"`ext param {slot} {idx}`, got {len(matches)}: {out}"
            )
        return matches[0][1]

    def ext_param_int(self, slot: int, idx: int, name: str | None = None) -> int:
        """Integer value of a uint32/bool ext param (tolerates the trailing
        ` (0x..)` the shell appends to bools). `name`, if given, is
        cross-checked against the printed ParamName — see ext_param.

        Matches with re.fullmatch on the first whitespace-delimited token, NOT
        re.match: a bare `re.match(r"-?\\d+")` anchors only at the start, so a
        COLOR value like `0x00ff80` would decode to its leading `0` and this
        would silently return 0 — the worst failure for a persistence pin, a
        before/after-reboot compare of a reset color reading `0 == 0` and
        passing green (PR #365 review). fullmatch makes a non-integer value
        fail the assert loudly instead."""
        raw = self.ext_param(slot, idx, name=name)
        tok = raw.split()[0]  # drop the trailing ' (0x7b)' on bool output
        m = re.fullmatch(r"-?\d+", tok)
        assert m, f"ext param {slot}/{idx} is not an integer: {raw!r}"
        return int(m.group(0))

    def ext_stats(self) -> dict[str, dict[str, int]]:
        """`ext stats` → {name: {ticks, cpu_min/avg/max, wall_min/avg/max}}."""
        stats: dict[str, dict[str, int]] = {}
        current: str | None = None
        for line in self.exec("ext stats"):
            s = line.strip()
            m = re.match(r"^\[\d+\]\s+'(.*)':\s+(\d+)\s+ticks", s)
            if m:
                current = m.group(1)
                stats[current] = {"ticks": int(m.group(2))}
                continue
            m = re.match(r"^(cpu|wall)\s+min/avg/max\s+=\s+(\d+)/(\d+)/(\d+)\s+us", s)
            if m and current is not None:
                kind = m.group(1)
                stats[current][f"{kind}_min"] = int(m.group(2))
                stats[current][f"{kind}_avg"] = int(m.group(3))
                stats[current][f"{kind}_max"] = int(m.group(4))
        return stats

    def led_stats(self) -> dict:
        """`led_stats` → {frames, target_us, interval_min/avg/max_us,
        late_frames, work_max_us, worst_wall/self/other/idle_us,
        worst_label, overruns}. (Format: led_controller.cpp cmd_led_stats.)
        """
        out = self.exec("led_stats")
        stats: dict = {}
        for line in out:
            s = line.strip()
            for key, pat in (
                ("frames", r"^frames:\s+(\d+)"),
                ("target_us", r"^target:\s+(\d+)\s+us/frame"),
                ("late_frames", r"^late \(>\d+x\):\s+(\d+)\s+frame"),
                ("work_max_us", r"^work max:\s+(\d+)\s+us"),
                ("held_frames", r"^held frames:\s+(\d+)"),
                ("overruns", r"^overruns:\s+(\d+)"),
            ):
                m = re.match(pat, s)
                if m:
                    stats[key] = int(m.group(1))
            m = re.match(r"^interval:\s+min (\d+) us\s+avg (\d+) us\s+max (\d+) us", s)
            if m:
                stats["interval_min_us"] = int(m.group(1))
                stats["interval_avg_us"] = int(m.group(2))
                stats["interval_max_us"] = int(m.group(3))
            m = re.match(
                r"^worst segment:\s+(\d+) us wall = (\d+) us self \+ (\d+) us "
                r"other-thread \+ (\d+) us idle\s+in '(.*?)'",
                s,
            )
            if m:
                stats["worst_wall_us"] = int(m.group(1))
                stats["worst_self_us"] = int(m.group(2))
                stats["worst_other_us"] = int(m.group(3))
                stats["worst_idle_us"] = int(m.group(4))
                stats["worst_label"] = m.group(5)
        return stats

    def shuffle_status(self) -> dict:
        """`anim shuffle status` → {enabled, min_s, max_s, grace_s, max_grace_s}."""
        out = self.exec("anim shuffle status")
        for line in out:
            m = re.search(
                r"shuffle:\s+(on|off), min:\s+(\d+) s, max:\s+(\d+) s, "
                r"grace:\s+(\d+) s \(max (\d+) s\)",
                line,
            )
            if m:
                return {
                    "enabled": m.group(1) == "on",
                    "min_s": int(m.group(2)),
                    "max_s": int(m.group(3)),
                    "grace_s": int(m.group(4)),
                    "max_grace_s": int(m.group(5)),
                }
        raise AssertionError(f"could not parse `anim shuffle status`: {out}")

    def ext_faults(self) -> list[dict]:
        """`ext faults` → [{slot, name, what, count, params_reset, state}].

        Empty list when the firmware prints "no extension faults recorded".
        """
        records: list[dict] = []
        cur: dict | None = None
        for line in self.exec("ext faults"):
            s = line.strip()
            m = re.match(r"^\[(\d+)\]\s+'(.*)':\s+(.*)$", s)
            if m:
                cur = {
                    "slot": int(m.group(1)),
                    "name": m.group(2),
                    "what": m.group(3),
                    "count": None,
                    "params_reset": None,
                    "state": None,
                }
                records.append(cur)
                continue
            if cur is None:
                continue
            m = re.search(r"(\d+)\s+time\(s\) since clear", s)
            if m:
                cur["count"] = int(m.group(1))
            m = re.match(r"^params reset to manifest defaults:\s+(yes|no)", s)
            if m:
                cur["params_reset"] = m.group(1) == "yes"
            m = re.match(r"^currently:\s+(.*)$", s)
            if m:
                cur["state"] = m.group(1)
        return records

    def settings_keys(self) -> list[str]:
        return [
            line.strip()
            for line in self.exec("settings list", timeout=30.0)
            if line.strip() and "/" in line
        ]

    def uptime_ms(self) -> int:
        for line in self.exec("kernel uptime"):
            m = re.search(r"(\d+)\s*ms", line)
            if m:
                return int(m.group(1))
        raise AssertionError("could not parse `kernel uptime`")
