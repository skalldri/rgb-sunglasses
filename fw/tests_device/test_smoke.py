"""Smoke tier: fast, read-only sanity of every major shell surface.

Each test is one subsystem's "is it alive and shaped right" check — every
command must succeed (retval 0, enforced by rgb.exec) AND print output
matching its known format. ~1 minute total, no reboots, no state changes.
"""

from __future__ import annotations

import re

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.smoke

# Every animation name `anim set` accepts on proto0 (its SHELL_SUBCMD_DICT set
# in fw/src/pattern_controller.cpp), plus extension animations reported by
# name. Used for a tolerant "is a plausible animation" check — the strict
# boot-default assertion lives in test_reset_and_settings.py, where the
# preceding reboot is controlled.
KNOWN_ANIMATIONS = {
    "none", "zigzag", "text", "rainbow", "my_eyes", "beat", "fft_bars",
    "glim_player", "matrix_code", "tilt", "pulse",
}


def test_kernel_version(rgb: RgbShell):
    out = rgb.exec("kernel version")
    assert any("Zephyr version" in line for line in out), out


def test_serial_identity(rgb: RgbShell):
    out = rgb.exec("serial print")
    assert out, "`serial print` printed nothing"


def test_bq_status_format(rgb: RgbShell):
    kv = rgb.bq_status()
    for key in ("VBAT", "IBAT", "VBUS", "IBUS", "CHG_STAT", "EN_CHG"):
        assert key in kv, f"missing {key} in `power bq status`: {kv}"


def test_bq_limits_format(rgb: RgbShell):
    kv = rgb.bq_limits()
    # AUTO_INDET_EN included (#335): ungated presence check so a firmware that
    # dropped the field is caught even on rigs where the charging tests skip.
    for key in ("ICHG", "IINDPM", "VINDPM", "VBAT_PRESENT", "VBUS_PRESENT", "AUTO_INDET_EN"):
        assert key in kv, f"missing {key} in `power bq limits`: {kv}"


def test_power_policy_format(rgb: RgbShell):
    kv = rgb.policy()
    for key in ("user_enable", "effective_enable", "gated", "vbat_present", "vbus_present"):
        assert key in kv, f"missing {key} in `power policy`: {kv}"


def test_pd_contract_responds(rgb: RgbShell):
    kv = rgb.pd_contract()
    assert "connected" in kv, f"`power pd contract` missing connected=: {kv}"


def test_bt_state_responds(rgb: RgbShell):
    out = rgb.exec("bt_state")
    assert any(re.search(r"state|advertis|connect", line, re.I) for line in out), out


def test_ext_list_clean(rgb: RgbShell):
    slots = rgb.ext_list()
    faulted = [s["name"] for s in slots if s["faulted"]]
    assert not faulted, f"extensions in FAULTED state at baseline: {faulted}"


def test_glim_list_responds(rgb: RgbShell):
    # Content is asserted by the provisioning gate; here only "responds sanely".
    rgb.exec("glim list")


def test_anim_get_plausible(rgb: RgbShell):
    # "unknown" is accepted deliberately: cmd_anim_get switches over the
    # built-in Animation enum only, so an ACTIVE EXTENSION (ids 0x40+) always
    # prints "unknown" — correct firmware behavior, not a failure (PR #341
    # review). Extension displayNames are never printed by `anim get`; if
    # cmd_anim_get ever learns to resolve extension ids, drop "unknown" here.
    name = rgb.anim_get()
    assert name in KNOWN_ANIMATIONS | {"unknown"}, f"`anim get` returned {name!r}"


def test_coredump_manager_clean(rgb: RgbShell):
    out = rgb.exec("coredump_mgr status")
    text = " ".join(out)
    assert "no dumps" in text, (
        f"board is carrying uncollected crash dumps: {out} — collect with "
        "fw/scripts/coredump-fetch.sh --delete, reboot, re-run"
    )


def test_sound_works_without_audio_debug(rgb: RgbShell):
    # Issue #284: CONFIG_APP_AUDIO_DEBUG defaults n (33 KB RAM). The always-on
    # sound surface must keep working without it.
    rgb.exec("sound dsp params")
    rgb.exec("sound agc status")


def test_led_stats_responds(rgb: RgbShell):
    out = rgb.exec("led_stats")
    assert any("frames" in line.lower() or "overrun" in line.lower() for line in out), out


def test_reset_cause_responds(rgb: RgbShell):
    out = rgb.exec("hwinfo reset_cause show")
    text = " ".join(out)
    assert "reset caused by" in text or "No reset cause set" in text, out


def test_stacks_parse(rgb: RgbShell):
    stacks = rgb.stacks()
    assert len(stacks) >= 10, f"parsed only {len(stacks)} threads: {sorted(stacks)}"
    assert "main" in stacks, sorted(stacks)
