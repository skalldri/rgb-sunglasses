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

# "0x20001234 thread_name (real size 2048):  unused 1234  usage 814 / 2048 (39 %)"
_STACKS_RE = re.compile(
    r"^0x[0-9a-fA-F]+\s+(?P<name>\S+(?: \d+)?)\s*"
    r"\(real size\s+(?P<size>\d+)\):\s+unused\s+\d+\s+"
    r"usage\s+(?P<used>\d+)\s+/\s+\d+\s+\((?P<pct>\d+)\s*%\)"
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

        Boot floods the console (llext relocation logs, USB bring-up) for
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

    def _exec_with_retry(self, cmd: str, timeout: float | None) -> list[str]:
        """One shell exchange, retried on echo-smear timeouts.

        An async log line can interleave with the command's echo so the
        harness never matches it (the same failure mode the serial-MCP
        plugin's _run_command retries around). Flush + resend converges on
        the second try in practice.
        """
        last_exc: Exception | None = None
        for attempt in range(3):
            try:
                return self.shell.exec_command(cmd, timeout=timeout or 10.0)
            except TwisterHarnessTimeoutException as exc:
                last_exc = exc
                logger.warning("exchange %r timed out (attempt %d); flush + retry", cmd, attempt + 1)
                self.dut.write(b"\x03")
                time.sleep(0.2)
                self.dut.clear_buffer()
        raise AssertionError(f"exchange {cmd!r} failed after 3 attempts: {last_exc}")

    def exec(self, cmd: str, timeout: float | None = None, check: bool = True) -> list[str]:
        """Run a command; return its filtered output lines.

        With check=True (default), also runs `retval` and fails the test if
        the command's return value was non-zero.
        """
        self.dut.write(b"\x03")
        time.sleep(0.05)
        self.dut.clear_buffer()
        out = self.shell.get_filtered_output(self._exec_with_retry(cmd, timeout))
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

    def reboot(self, cold: bool = False, timeout: float = 90.0) -> list[str]:
        """Reboot the board and wait for the shell to come back.

        Returns whatever console output was captured while waiting — a
        best-effort boot backlog (the USB CDC console only starts streaming
        once it re-enumerates, so the earliest boot lines may be missing).
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
