"""Charger-policy regression tests that depend on the power configuration.

These carry `requires_*` markers by design (the README's exception list):
each has NO meaningful assertion outside its configuration. On a rig without
a battery, #142 is the test that RUNS and the others skip — the coverage
trade documented in the power-test design principle.

Regressions pinned:
- #141  The BQ25792 WATCHDOG field is 3 bits, not 2 — the old decode left
        "disable" arming a 20 s watchdog whose expiry silently reverted
        ICHG to the 2000 mA POR default behind the policy's back.
- #142  Charge enable must be gated on battery presence; re-applying EN_CHG
        with no pack caused a BATOVP → brownout reboot loop.
"""

from __future__ import annotations

import time

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.integration


@pytest.mark.requires_charging
def test_watchdog_stays_disabled(rgb: RgbShell):
    limits = rgb.exec("power bq limits")
    assert any("WATCHDOG=disabled" in line for line in limits), (
        f"I2C watchdog not disabled at rest (#141 decode regression?): "
        f"{[line for line in limits if 'WATCHDOG' in line]}"
    )
    before = rgb.policy()

    # Strictly longer than BOTH the 20 s AND 40 s timers a #141-style
    # mis-decode could leave armed (kWatchdog in power.cpp: the field's
    # realistic wrong values; 80/160 s are impractical to cover in an
    # integration test). If a watchdog were secretly running, expiry resets
    # ICHG to the 2000 mA POR value and bumps wd_redisables.
    time.sleep(50.0)

    after = rgb.policy()
    assert after["wd_redisables"] == before["wd_redisables"], (
        f"watchdog expired during idle (wd_redisables "
        f"{before['wd_redisables']} -> {after['wd_redisables']}) — the #141 "
        f"'disabled' decode is not actually disabling it"
    )
    kv = rgb.parse_kv(rgb.exec("power bq limits"))
    assert kv["ICHG"] == after["ichg_target"], (
        f"ICHG register ({kv['ICHG']} mA) diverged from policy target "
        f"({after['ichg_target']} mA) across idle — POR revert signature"
    )
    status = rgb.bq_status()
    assert status["EN_CHG"] == 1, f"charging dropped out during idle: {status}"


@pytest.mark.requires_no_battery
def test_no_battery_charge_gating(rgb: RgbShell):
    policy = rgb.policy()
    assert policy["vbat_present"] == 0
    assert policy["gated"] == 1, f"charging not gated without a battery: {policy}"
    assert policy["effective_enable"] == 0, (
        f"EN_CHG effective with no pack — the #142 BATOVP/brownout setup: {policy}"
    )

    # The #142 failure mode was a reboot LOOP: uptime must climb monotonically.
    t0 = rgb.uptime_ms()
    time.sleep(60.0)
    t1 = rgb.uptime_ms()
    assert t1 > t0 + 55_000, (
        f"uptime went {t0} -> {t1} ms across a 60 s wait — board rebooted "
        f"(brownout loop?)"
    )


def test_auto_indet_disabled(rgb: RgbShell):
    """#169 (the fix itself, config-INDEPENDENT): charger_policy_boot_init
    clears BQ25792 AUTO_INDET_EN unconditionally at boot — before any battery
    or VBUS read — because the D+/D- pins are NC and BC1.2 must not probe
    them. So this holds on EVERY board in EVERY power configuration, which is
    exactly why it is NOT `requires_charging`: a batteryless CI rig (where
    the charging test below skips) is precisely where a reintroduced #169
    would otherwise ship green.
    """
    kv = rgb.parse_kv(rgb.exec("power bq limits"))
    # Presence check first: pre-#335 firmware has no such field, and
    # parse_kv omits absent keys — distinguish "the fix regressed" from
    # "you're testing a stale image" (HIL runs don't always reflash).
    assert "AUTO_INDET_EN" in kv, (
        "`power bq limits` has no AUTO_INDET_EN field — firmware predates "
        f"#335; reflash before trusting this test. Parsed: {kv}"
    )
    assert kv["AUTO_INDET_EN"] == 0, (
        f"AUTO_INDET_EN={kv['AUTO_INDET_EN']} — the #169 boot-time clear "
        f"regressed; BC1.2 will probe the NC D+/D- pins and can latch 'not "
        f"qualified adaptor', blocking all charging: {kv}"
    )


@pytest.mark.requires_charging
def test_charging_actually_charges(rgb: RgbShell):
    """#169 observable consequence: with battery + VBUS + enable, charging
    actually proceeds. (The fix itself is covered config-independently by
    test_auto_indet_disabled above.) Note VBUS_STAT==8 was observed on a
    HEALTHY charging board (2026-08-11), so the catalogue's VBUS_STAT!=8
    expectation is wrong; do not add it.
    """
    kv = rgb.parse_kv(rgb.exec("power bq limits"))
    # CHG_STAT 0 = not charging; 1-6 = charging phases; 7 = termination done.
    assert kv["CHG_STAT"] != 0, (
        f"CHG_STAT=0 (not charging) despite battery+VBUS+enable: {kv}"
    )
    assert kv["PG"] == 1, f"power-good not set while on VBUS: {kv}"
