"""Soak tier: drift and timing regressions that only appear under sustained
load. Every test here runs minutes, not seconds — `--tier soak`, never in the
default set (twister scenario app.device.soak, slow: true).

Regressions pinned:
- #267/#271/#312  Frame pacing: the display thread must hold its 33.3 ms
        budget over minutes of real rendering — overruns, multi-second
        segment stalls, and PDM buffer-allocation failures all appeared
        only under sustained load.
- #304/#307  Extension phase-accumulator drift: an unbounded accumulator
        fed to sinf() grows the fdlibm reduction cost without bound. Stats
        reset on activation, and the collapse took >300 s to manifest — a
        short test structurally cannot see it.
- #257  Shuffle must not hard-cut a long clip mid-play: with bad_apple in
        LoopOne, the switch-away has to wait for the clip's good-moment
        (its end), not fire at max+grace.
- #234  GLIM format 4 (Lz4PerFrameRgb24): a full playthrough of 4096.glim
        with zero decode/seek errors.
"""

from __future__ import annotations

import re
import time

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.soak

# Names as printed by `ext list` (manifest displayNames). Plasma is the
# animation whose accumulator regressed in #307; it is a standalone-repo
# extension so not part of the provisioning baseline — hello (also sinf-
# based) is the fallback.
PLASMA = "Plasma"
HELLO = "Hello Extension"

# 4096.glim: 24 fps LZ4 clip, one pass ≈ 102 s (issue #96/#234).
GLIM_LZ4 = "4096.glim"
GLIM_LZ4_PASS_S = 115
# bad_apple.glim: 5258 frames @ 24 fps ≈ 215.6 s (issue #257).
GLIM_LONG = "bad_apple.glim"


def _collect_console(rgb: RgbShell, seconds: float) -> list[str]:
    """Drain console lines for a window, prompt-stripped (same rationale as
    the steady-state spam test: the redraw prefixes every async log line)."""
    rgb.dut.clear_buffer()
    lines: list[str] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            raw = rgb.dut.readline(timeout=2.0, print_output=False)
        except Exception:
            continue
        stripped = raw.replace("uart:~$", "").strip()
        if stripped:
            lines.append(stripped)
    return lines


def test_frame_pacing_soak(rgb: RgbShell):
    """#267/#271/#312: 5 minutes of real rendering load (GLIM playback —
    FAT reads + decode + render), then the led_stats budget gates."""
    rgb.exec("anim set glim_player")
    try:
        rgb.exec("led_stats reset")
        console = _collect_console(rgb, 300.0)

        errors = [ln for ln in console if "<err>" in ln or "Failed to allocate" in ln]
        assert not errors, f"error lines during the soak: {errors[:5]}"

        s = rgb.led_stats()
        # ~9000 frames at 33.3 ms; require 90% to prove rendering actually ran.
        assert s["frames"] > 8100, f"only {s['frames']} frames in 300 s: {s}"
        assert s["overruns"] == 0, f"frame overruns during soak (#267): {s}"
        assert s["work_max_us"] < s["target_us"], (
            f"work max {s['work_max_us']} µs exceeds the {s['target_us']} µs "
            f"frame budget (#267): {s}"
        )
        # #312's stalls were 0.6-1.5 s; the catalogue ceiling is 100 ms.
        assert s["worst_wall_us"] < 100_000, (
            f"segment stall {s['worst_wall_us']} µs in '{s['worst_label']}' (#312): {s}"
        )
    finally:
        rgb.exec("anim set zigzag", check=False)


def test_ext_cpu_soak(rgb: RgbShell):
    """#304/#307: >300 s of continuous extension ticking; CPU max must stay
    flat. The #307 collapse (3.4 ms → 25 ms cpu max, render rate 90→28 Hz)
    is invisible before ~200 s because sinf's argument has to grow first."""
    names = {s["name"] for s in rgb.ext_list()}
    target = PLASMA if PLASMA in names else HELLO
    if target not in names:
        pytest.fail(f"neither {PLASMA} nor {HELLO} installed: {sorted(names)}")

    slot = [s for s in rgb.ext_list() if s["name"] == target][0]["slot"]
    rgb.exec(f"ext select {slot}")
    try:
        deadline = time.monotonic() + 320.0
        while time.monotonic() < deadline:
            time.sleep(30.0)
            cur = [s for s in rgb.ext_list() if s["name"] == target][0]
            assert not cur["faulted"], (
                f"{target} faulted mid-soak: {rgb.ext_faults()}"
            )
        stats = rgb.ext_stats()[target]
        assert stats["ticks"] > 5000, f"implausibly few ticks after 320 s: {stats}"
        # Healthy plasma: 3.4/3.6/4.1 ms; the #307 regression hit 25 ms max.
        assert stats["cpu_max"] < 5000, (
            f"{target} cpu max {stats['cpu_max']} µs after {stats['ticks']} "
            f"ticks — phase-accumulator drift (#304/#307): {stats}"
        )
    finally:
        rgb.exec("anim set zigzag", check=False)


def test_shuffle_waits_for_long_clip(rgb: RgbShell):
    """#257: with bad_apple in LoopOne, shuffle's switch-away must wait for
    the clip's good-moment (~215 s in) — the broken behavior hard-cut at
    max+grace, and the naive fix converged on the midpoint (60-150 s band).
    """
    st = rgb.shuffle_status()
    # The wanted-switch point is min..max after activation, then grace-bound
    # waiting for a good moment. If the device's persisted max+grace already
    # reaches past the clip-end region, a mid-clip cut and a legitimate
    # timed switch are indistinguishable — the test cannot discriminate.
    if st["max_s"] + st["grace_s"] > 170:
        pytest.skip(
            f"persisted shuffle timing (max {st['max_s']} s + grace "
            f"{st['grace_s']} s) overlaps the clip-end window; cannot "
            f"discriminate a #257 regression on this configuration"
        )

    orig_glim = rgb.glim_selected()
    rgb.exec("anim set glim_player")
    rgb.glim_select_name(GLIM_LONG)
    rgb.exec("glim set_loop_mode loop_one")
    try:
        t0 = time.monotonic()
        rgb.exec("anim shuffle on")
        dwell = None
        while time.monotonic() - t0 < 300.0:
            if rgb.anim_get() != "glim_player":
                dwell = time.monotonic() - t0
                break
            time.sleep(5.0)
        assert dwell is not None, (
            "shuffle never switched away within 300 s — stuck waiting for a "
            "good moment that never signals?"
        )
        assert dwell > 170.0, (
            f"shuffle cut bad_apple {dwell:.0f} s in — mid-clip hard cut "
            f"(#257): the good-moment wait is gone"
        )
        assert dwell < 270.0, (
            f"switch at {dwell:.0f} s — well past the ~215 s clip end; "
            f"good-moment signaling from the glim player looks broken"
        )
    finally:
        rgb.exec("anim shuffle off", check=False)
        rgb.exec("anim set zigzag", check=False)
        if orig_glim:
            rgb.glim_select_name(orig_glim)


def test_glim_lz4_full_pass(rgb: RgbShell):
    """#234: one complete playthrough of the LZ4 (format 4) asset with zero
    decode/seek errors — the regression rendered RGB bytes as a 1-bit
    bitmap because format 4 fell into the mono branch."""
    orig_glim = rgb.glim_selected()
    rgb.exec("anim set glim_player")
    rgb.glim_select_name(GLIM_LZ4)
    rgb.exec("glim set_loop_mode loop_one")
    try:
        console = _collect_console(rgb, GLIM_LZ4_PASS_S)
        # Only ERROR-level lines and genuine failure words count — the
        # decoder's own INFO banner ("glim_decoder: GLIM opened: ... format
        # 4") matches a naive 'decode' substring and false-alarmed the first
        # hardware run. That banner is positive evidence, asserted below.
        bad = [
            ln
            for ln in console
            if "<err>" in ln
            or re.search(r"fail|error|invalid|corrupt", ln, re.I)
        ]
        assert not bad, f"decoder complaints during the LZ4 pass: {bad[:5]}"
        opened = [ln for ln in console if "GLIM opened" in ln and "format 4" in ln]
        assert opened, (
            f"never saw the format-4 open banner — did the LZ4 path engage? "
            f"({GLIM_LZ4} selected, {len(console)} console lines)"
        )
        assert rgb.anim_get() == "glim_player", (
            "playback did not survive the full pass"
        )
    finally:
        rgb.exec("anim set zigzag", check=False)
        if orig_glim:
            rgb.glim_select_name(orig_glim)
