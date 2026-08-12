"""BLE-adjacent and extension-host regression tests (no phone required).

Regressions pinned:
- #225/#208  Advertising must not start before extension loading finishes —
        the app used to connect and read unpopulated GATT. The gate is also
        bounded (≤30 s) and must release on error paths.
- #89   activate()/deactivate() raced tick() with no lock — aborting the
        sandbox mid-handshake and reusing the thread/stack while the old one
        was still unwinding.
- #276/#278  The per-tick budget is CPU time, never wall time. Charging
        other threads' preemption to the extension caused spurious faults.
- #110 (house rule)  No info-level logs in steady-state/per-tick paths. The
        original notify-flood printks are compiled out; this asserts the
        general rule so any reintroduced per-tick spam fails loudly.
"""

from __future__ import annotations

import time

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.integration

HELLO = "Hello Extension"


def _find_ext(rgb: RgbShell, name: str) -> dict:
    slots = rgb.ext_list()
    matches = [s for s in slots if s["name"] == name]
    assert matches, f"extension {name!r} not loaded: {[s['name'] for s in slots]}"
    return matches[0]


def test_adv_gated_on_ext_load(rgb: RgbShell, device_state: dict):
    """Reboot, then poll: when advertising is FIRST observed, the extension
    registry must already be fully populated (the #225 gate runs strictly
    before the boot-time advertising start), within the 30 s bound (#208).
    Console-log ordering is deliberately not used — the CDC console comes up
    too late to see the early half of the race.
    """
    if "CONNECTED" in " ".join(rgb.exec("bt_state")):
        pytest.skip("a central is connected; advertising gate not observable")
    expected_ext = {s["name"] for s in device_state["ext"]}

    # settle=False is load-bearing: reboot()'s default settle barrier only
    # returns AFTER extension registration completes, which would make this
    # test structurally unable to observe the #225 race (review finding on
    # this very test). Polling uses probe(), not exec(): exec's echo-smear
    # retry backoff against the boot flood could eat the whole measurement
    # window. The 30 s bound is DEVICE uptime, so it means what it says —
    # host-side clocks start ~10 s of enumeration late (both PR #346
    # review findings).
    try:
        rgb.reboot(settle=False)
        while True:
            m = rgb.probe("bt_state", r"Advertising:\s*(yes|no)")
            if m and m.group(1) == "yes":
                break
            up = rgb.probe_uptime_ms()
            assert up is None or up < 30_000, (
                f"advertising not started at {up} ms device uptime — the "
                f"#208 gate is stuck (or never released on an error path)"
            )
            time.sleep(1.0)

        loaded = {s["name"] for s in rgb.ext_list()}
        assert loaded == expected_ext, (
            f"advertising observed with an incomplete extension registry "
            f"(#225): loaded {sorted(loaded)}, expected {sorted(expected_ext)}"
        )
    finally:
        # Later tests assume a settled board — guarantee it even on failure,
        # or one real failure cascades into unrelated-looking ones.
        rgb.wait_boot_settled()


@pytest.mark.requires_ext(HELLO)
def test_ext_select_race(rgb: RgbShell):
    slots = rgb.ext_list()
    if len(slots) < 2:
        pytest.skip("need ≥2 extensions for the alternation")
    a, b = slots[0]["slot"], slots[1]["slot"]

    try:
        for i in range(25):
            rgb.exec(f"ext select {a}")
            rgb.exec(f"ext select {b}")
        # Let the last activation actually run some ticks.
        time.sleep(2.0)
        after = rgb.ext_list()
        faulted = [s["name"] for s in after if s["faulted"]]
        assert not faulted, (
            f"extension(s) FAULTED after 50 alternating selects (#89 "
            f"activate/deactivate race): {faulted}"
        )
        stats = rgb.ext_stats()
        active = [s for s in after if s["active"]]
        assert active, f"nothing active after the alternation: {after}"
        assert stats.get(active[0]["name"], {}).get("ticks", 0) > 0, (
            f"active extension recorded no ticks: {stats}"
        )
    finally:
        rgb.exec("anim set zigzag")  # deactivate; leave the board on default


@pytest.mark.requires_ext(HELLO)
def test_ext_cpu_budget_not_wall(rgb: RgbShell):
    slot = _find_ext(rgb, HELLO)["slot"]
    rgb.exec(f"ext select {slot}")
    try:
        # ≥5000 ticks at the ~33 ms render rate ≈ 3 min. Poll rather than
        # sleep blind so a fault aborts fast.
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            stats = rgb.ext_stats().get(HELLO, {})
            current = _find_ext(rgb, HELLO)
            assert not current["faulted"], (
                f"extension FAULTED during steady ticking (#276 spurious "
                f"budget fault?): stats={stats}"
            )
            if stats.get("ticks", 0) >= 5000:
                break
            time.sleep(10.0)
        else:
            pytest.fail(f"never reached 5000 ticks in 5 min: {stats}")

        # Both dimensions must be reported separately — printing only one
        # number is what made #276 misdiagnosable.
        for key in ("cpu_max", "wall_max", "cpu_avg", "wall_avg"):
            assert key in stats, f"`ext stats` missing {key}: {stats}"
        # The budget is enforced against CPU. hello is a light animation:
        # half the 50 ms budget is an enormous margin, while a regression
        # that charges preemption to the extension blows straight past it.
        assert stats["cpu_max"] < 25_000, (
            f"cpu max {stats['cpu_max']} µs — implausible for hello; wall "
            f"time being charged as CPU? (#276) full stats: {stats}"
        )
    finally:
        rgb.exec("anim set zigzag")


def test_no_steady_state_log_spam(rgb: RgbShell):
    if "CONNECTED" in " ".join(rgb.exec("bt_state")):
        pytest.skip("a central is connected; baseline console not quiet")
    rgb.exec("anim set text")  # scrolling text ticks continuously
    try:
        rgb.dut.clear_buffer()
        lines: list[str] = []
        deadline = time.monotonic() + 60.0
        while time.monotonic() < deadline:
            try:
                lines.append(rgb.dut.readline(timeout=2.0, print_output=False))
            except Exception:
                pass  # quiet — that's the point
        # STRIP the redrawn prompt from each line rather than dropping lines
        # that contain it: the shell redraws "uart:~$ " after every async
        # log line, so spam arrives as "uart:~$ <log>" — a contains-filter
        # discarded virtually everything and made this check inert (PR #346
        # review).
        noise = [
            stripped
            for ln in lines
            if (stripped := ln.replace("uart:~$", "").strip())
        ]
        assert len(noise) < 10, (
            f"{len(noise)} console lines in 60 s of steady-state rendering — "
            f"per-tick log spam (PR #110 rule). First few: {noise[:5]}"
        )
    finally:
        rgb.exec("anim set zigzag")
