"""Thread-map and stack-occupancy regression tests.

- Priority table (#269/#270/#271): every application thread's priority is a
  Kconfig symbol with BUILD_ASSERTed ordering invariants; this pins the
  RUNTIME map against fw/docs/threading.md (the single system-wide map) so a
  bad override or a silently-ignored Kconfig default (see fw/CLAUDE.md) shows
  up as a red test instead of a timing mystery.

- Stack occupancy (#326, resolves #328): stack creep previously had no
  detector other than a human running `kernel thread stacks` — the
  charger_status thread reached 95% of its 1024 B stack before anyone
  noticed. These are high-water marks since boot, measured after the suite
  has already exercised the shell surface.
"""

from __future__ import annotations

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.integration

# Expected priorities, from fw/docs/threading.md's table (cross-checked there
# against a real `kernel thread list` dump). Only threads with stable,
# discoverable names are pinned; extra threads are ignored, MISSING pinned
# threads fail. Cooperative band = negative numbers.
EXPECTED_PRIORITIES = {
    "sysworkq": -1,
    "main": 0,
    "led_display_thread": 2,
    "pattern_controller_thread": 4,
    "audio_dsp_thread": 5,
    "bt_thread": 6,
    "imu_thread": 7,
    "status_led_thread": 8,
    "charger_status_thread": 8,
    "tps25750_wq": 10,
    "persist_wq": 14,
    "coredump_wq": 14,
    "mcuboot_upd_wq": 14,
    "shell_uart": 14,
    "idle": 15,
}

# Per-thread stack high-water ceilings (percent). Sources: fw/docs/threading.md
# stack table + the #325/#326 measurements. The generic ceiling exists so a
# thread nobody thought to pin still can't creep to the edge unnoticed —
# charger_status hit 95% before #326.
STACK_PCT_CEILINGS = {
    "main": 50,             # ≤7700 B of 16384 even after a boot-time f_mkfs (#325/#105)
    "shell_uart": 80,       # 6656 B sized for `fatfs reformat` (5060 B measured)
    "charger_status_thread": 60,  # 2048 B post-#326; carries the bridged BQ transaction
}
GENERIC_PCT_CEILING = 85
# Threads whose stacks are sized razor-thin upstream and are not ours to
# budget (logging's is by design nearly full when CONFIG_LOG_MODE_DEFERRED).
STACK_CHECK_EXEMPT = {"logging"}


def test_thread_priority_table(rgb: RgbShell):
    actual = rgb.thread_priorities()
    assert len(actual) >= 10, f"parsed too few threads: {actual}"

    missing = [name for name in EXPECTED_PRIORITIES if name not in actual]
    assert not missing, (
        f"expected threads absent from `kernel thread list`: {missing} "
        f"(present: {sorted(actual)})"
    )
    diffs = {
        name: (actual[name], want)
        for name, want in EXPECTED_PRIORITIES.items()
        if actual[name] != want
    }
    assert not diffs, (
        "thread priorities diverge from fw/docs/threading.md "
        "(actual, expected): " + repr(diffs)
    )


def test_stack_occupancy(rgb: RgbShell):
    stacks = rgb.stacks()
    assert "main" in stacks, f"no main thread in `kernel thread stacks`: {sorted(stacks)}"

    offenders = []
    for name, info in stacks.items():
        if name in STACK_CHECK_EXEMPT or name.startswith("IRQ"):
            continue
        ceiling = STACK_PCT_CEILINGS.get(name, GENERIC_PCT_CEILING)
        if info["pct"] > ceiling:
            offenders.append(
                f"{name}: {info['used']}/{info['size']} B = {info['pct']}% "
                f"(ceiling {ceiling}%)"
            )
    assert not offenders, (
        "stack high-water ceiling exceeded (#328's regression watch):\n  "
        + "\n  ".join(offenders)
    )

    # The #326 class specifically: absolute headroom, not just percentage —
    # a small stack at a modest percentage can still be one deep call from
    # overflow. 128 B is less than one printk frame.
    thin = [
        f"{name}: only {info['size'] - info['used']} B unused of {info['size']}"
        for name, info in stacks.items()
        if not name.startswith("IRQ")
        and name not in STACK_CHECK_EXEMPT
        and (info["size"] - info["used"]) < 128
    ]
    assert not thin, "threads within 128 B of overflow:\n  " + "\n  ".join(thin)
