"""
Regression tests for re-pair.py's answer_pairing() UART-driven loop — specifically the
issue #232 flow tolerance: with CONFIG_BT_SMP_SC_ONLY the firmware REJECTS a raced-in
unauthenticated (Just Works) pairing attempt ("Pairing failed" on the UART, connection
still up) and Android retries with a MITM passkey pairing on the same connection. The
old loop treated any "Pairing failed" as a fatal attempt failure; the new loop rebases
its scan offset past the failure, drops any pre-failure passkey, and keeps answering.

No hardware: SerialWatcher is replaced by an in-memory staged buffer (fed one chunk per
UI poll, so the loop observes events in UART order, not all at once), and UI/adb are
recorded fakes in the style of test_re_pair_forget.py.

Run:  pytest scripts/tests/
"""
import importlib.util
import os
import re

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))

# re-pair.py has a hyphen -> load it by path rather than `import`.
_spec = importlib.util.spec_from_file_location("re_pair", os.path.join(_HERE, "..", "re-pair.py"))
re_pair = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(re_pair)


@pytest.fixture(autouse=True)
def _no_sleep(monkeypatch):
    monkeypatch.setattr(re_pair.time, "sleep", lambda s: None)


class _StagedWatch:
    """SerialWatcher stand-in: same offset/scan_after/passkey_after contract, but the
    buffer grows one staged chunk per feed() so tests control UART event ordering."""

    def __init__(self, stages):
        self.buf = b""
        self.stages = list(stages)

    def feed(self):
        if self.stages:
            self.buf += self.stages.pop(0)

    def offset(self):
        return len(self.buf)

    def scan_after(self, off, pattern):
        return re.search(pattern.encode() if isinstance(pattern, str) else pattern,
                         self.buf[off:])

    def passkey_after(self, off):
        found = re_pair.PASSKEY_RE.findall(self.buf[off:])
        return found[-1].decode() if found else None


class _FakeUi:
    """Feeds the watcher one stage per dump() (the loop polls UI once per idle pass) and
    shows a PIN EditText only while `pin_dialog` is set by the test scenario."""

    def __init__(self, watch):
        self.watch = watch
        self.pin_dialog = False

    def dump(self):
        self.watch.feed()
        return True

    def has(self, pattern):
        return self.pin_dialog and "EditText" in pattern

    def tap(self, pattern, ymin=0.0, ymax=1.0):
        return False


class _AdbRecorder:
    def __init__(self):
        self.calls = []

    def shell(self, *args, **kw):
        self.calls.append(args)
        return ""


def _rp(stages, timeout=5.0):
    rp = re_pair.RePair.__new__(re_pair.RePair)
    rp.watch = _StagedWatch(stages)
    rp.ui = _FakeUi(rp.watch)
    rp.a = _AdbRecorder()
    rp.connect_timeout = timeout
    rp.typed = []

    def enter(pk):
        rp.typed.append(pk)
        rp.ui.pin_dialog = False
        return True

    rp.enter_passkey = enter
    return rp


def _show_pin_after_passkey_stage(rp, passkey_stage_bytes):
    # Raise the PIN dialog at the same poll that delivers the passkey UART line,
    # mirroring the real ordering (board prints passkey ~when Android shows the dialog).
    orig_feed = rp.watch.feed

    def feed():
        before = rp.watch.stages[:1]
        orig_feed()
        if before and passkey_stage_bytes in before[0]:
            rp.ui.pin_dialog = True

    rp.watch.feed = feed


def test_happy_path_single_passkey_pairing():
    rp = _rp([
        b"bluetooth: Passkey for D0:49 (public): 654321\n",
        b"bluetooth: Pairing completed: D0:49 (public), bonded: 1\n",
    ])
    _show_pin_after_passkey_stage(rp, b"654321")
    assert rp.answer_pairing(0) is True
    assert rp.typed == ["654321"]


def test_rejected_just_works_then_mitm_retry_succeeds():
    # Issue #232: SC_ONLY rejects the unauthenticated attempt (no disconnect); the MITM
    # retry on the SAME connection must be answered. Old behavior returned False at the
    # "Pairing failed" line.
    rp = _rp([
        b"bluetooth: Pairing failed: D0:49 (public), reason 3\n",
        b"bluetooth: Passkey for D0:49 (public): 222333\n",
        b"bluetooth: Pairing completed: D0:49 (public), bonded: 1\n",
    ])
    _show_pin_after_passkey_stage(rp, b"222333")
    assert rp.answer_pairing(0) is True
    assert rp.typed == ["222333"]


def test_stale_pre_failure_passkey_is_never_reused():
    # A passkey printed before the failure must be dropped (pk reset on rebase); only
    # the fresh post-failure passkey may be typed.
    rp = _rp([
        b"bluetooth: Passkey for D0:49 (public): 111111\n"
        b"bluetooth: Pairing failed: D0:49 (public), reason 3\n",
        b"bluetooth: Passkey for D0:49 (public): 222333\n",
        b"bluetooth: Pairing completed: D0:49 (public), bonded: 1\n",
    ])
    _show_pin_after_passkey_stage(rp, b"222333")
    assert rp.answer_pairing(0) is True
    assert rp.typed == ["222333"]


def test_disconnect_reason_19_is_fatal():
    # Android dialog timeout/cancel: remote user terminated connection.
    rp = _rp([
        b"bluetooth: Pairing failed: D0:49 (public), reason 3\n",
        b"bluetooth: Disconnected (reason 19)\n",
    ])
    assert rp.answer_pairing(0) is False


def test_firmware_auth_fail_disconnect_is_fatal():
    # Issue #232 firmware disconnects (AUTH_FAIL) on failed escalation - any reason
    # code must be fatal for the attempt, not just 19.
    rp = _rp([b"bluetooth: Disconnected (reason 22)\n"])
    assert rp.answer_pairing(0) is False


def test_deadline_without_completion_returns_false():
    rp = _rp([], timeout=0.15)
    assert rp.answer_pairing(0) is False


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
