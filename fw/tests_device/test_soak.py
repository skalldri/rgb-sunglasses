"""Soak tier: drift and timing regressions that only appear under sustained
load. Every test here runs minutes, not seconds — `--tier soak`, never in the
default set (twister scenario app.device.soak, slow: true).

Preconditions (hard failures, not skips — fixable setup): shuffle must be
OFF (persisted; a shuffle switch mid-soak silently vacates the measurement)
and the board provisioned. Every test restores the glim selection AND loop
mode it touches (both persist to NVS).

Regressions pinned:
- #267/#271/#312  Frame pacing: the display thread must hold its frame
        budget over minutes of REAL rendering — overruns, multi-second
        segment stalls, and buffer-allocation failures appeared only under
        sustained load. Playback is proven, not assumed: the decoder's
        format banner is captured at selection and the frame count is
        checked against the device's own configured rate.
- #304/#307  Extension phase-accumulator drift: an unbounded accumulator
        fed to sinf() grows the fdlibm reduction cost without bound. Stats
        reset on activation and the collapse took >300 s to manifest — a
        short test structurally cannot see it.
- #257  Shuffle must not hard-cut a long clip mid-play: with bad_apple in
        LoopOne, the switch-away has to wait for the clip's good-moment
        (its end), not fire at max+grace.
- #234  GLIM format 4 (Lz4PerFrameRgb24): a full playthrough of 4096.glim
        with zero decoder errors.
"""

from __future__ import annotations

import contextlib
import re
import time

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.soak

PLASMA = "Plasma"
HELLO = "Hello Extension"

GLIM_LZ4 = "4096.glim"       # 24 fps LZ4 clip, one pass ≈ 102 s (#96/#234)
GLIM_LZ4_PASS_S = 115
GLIM_LONG = "bad_apple.glim"  # 5258 frames @ 24 fps ≈ 215.6 s (#257)

LOOP_MODES = ("loop_one", "play_all", "stop_after_one")


def _require_shuffle_off(rgb: RgbShell) -> None:
    """Hard fail (fixable setup): a persisted-on shuffle switches animations
    mid-soak, silently vacating whatever this tier is measuring (PR #349
    review — the dwell clock and the ext soak both depended on it)."""
    st = rgb.shuffle_status()
    assert not st["enabled"], (
        "shuffle is enabled (persisted) — turn it off (`anim shuffle off`) "
        "before running the soak tier; it invalidates every measurement here"
    )


def _persisted_rate_ms_x1000(rgb: RgbShell, key: str, default: int = 33300) -> int:
    """Persisted uint32 rate (ms x 1000) from the settings partition, or the
    firmware default when the key is not persisted. `settings read` prints a
    hex dump (4 bytes little-endian); a missing key prints an error line.

    Only those two outcomes are accepted — anything else (shell disabled,
    hexdump format change, empty exchange) fails LOUDLY rather than reading as
    "not persisted", which would let a real persisted divider slip past the
    precondition and surface as a misleading held-frames failure instead
    (PR #381 review)."""
    out = rgb.exec(f"settings read {key}", check=False)
    hexdump_seen = False
    error_seen = False
    for line in out:
        s = line.strip()
        m = re.match(r"^[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{2}\s+)+)", s)
        if m:
            b = bytes(int(x, 16) for x in m.group(1).split())
            if len(b) >= 4:
                return int.from_bytes(b[:4], "little")
            hexdump_seen = True
        # Zephyr settings shell not-found/read-error shapes ("not found",
        # "Failed to read...", "err -2/-ENOENT") = genuinely not persisted.
        # Only believed after scanning EVERY line and seeing no hexdump: an
        # untagged printk from another subsystem containing "err"/"failed"
        # must not short-circuit a real value into "not persisted"
        # (PR #381 review).
        if re.search(r"not found|failed|err(or)?\b|-2\b|ENOENT", s, re.IGNORECASE):
            error_seen = True
    if error_seen and not hexdump_seen:
        return default
    pytest.fail(
        f"unparseable `settings read {key}` output (neither a 4-byte hex dump "
        f"nor a not-found error) — cannot verify the divider precondition: {out!r}"
    )


def _require_default_divider(rgb: RgbShell) -> None:
    """Hard fail (fixable setup): a persisted render rate above the display
    rate makes the render thread run once per N consumed display frames
    (#379), so held_frames == (N-1)/N of frames BY DESIGN and the zero-held
    gate below would misfire on a correctly-behaving board."""
    render = _persisted_rate_ms_x1000(rgb, "appcfg/core/render_thread_rate_ms")
    display = _persisted_rate_ms_x1000(rgb, "appcfg/core/display_thread_rate_ms")
    assert display > 0 and round(render / display) <= 1, (
        f"persisted render/display rates {render}/{display} give a divider > 1 — "
        "delete appcfg/core/render_thread_rate_ms (settings delete) before the "
        "soak tier; the #379 held-frames gate assumes the default 1:1 pacing"
    )


def _glim_loop_mode(rgb: RgbShell) -> str | None:
    """Current persisted loop mode, or None if never persisted (default).

    The stored value is the dropdown string with the SELECTED token first;
    `settings read string` prints it raw, so the first known token wins.
    """
    out = rgb.exec("settings read string appcfg/glim_player/loop_mode", check=False)
    for line in out:
        for tok in LOOP_MODES:
            if line.strip().startswith(tok):
                return tok
    return None


@contextlib.contextmanager
def _glim_playback(rgb: RgbShell, clip: str):
    """Select `clip` in loop_one with PROOF the decoder opened it, restoring
    selection AND loop mode afterwards (both persist to NVS — a soak run
    must not permanently reconfigure a shared board; PR #349 review).

    The decoder banner is only emitted when the SELECTION CHANGES, so a
    different clip is selected first when needed, and the banner is captured
    from the select exchange itself via probe() — it fires before any
    post-hoc console window could open (PR #349 review).
    """
    orig_glim = rgb.glim_selected()
    orig_mode = _glim_loop_mode(rgb)

    rgb.exec("anim set glim_player")
    rgb.exec("glim set_loop_mode loop_one")
    names = rgb.glim_list()
    assert clip in names, f"{clip} not provisioned: {names}"
    if rgb.glim_selected() == clip:
        # Force a selection CHANGE so the open banner is emitted.
        other = next(n for n in names if n != clip)
        rgb.exec(f"glim select {names.index(other)}")
    m = rgb.probe(
        f"glim select {names.index(clip)}",
        r"GLIM opened:\s+\d+x\d+,\s+\d+ frames @ \d+ fps, format (\d+)",
        timeout=5.0,
    )
    assert m, f"no decoder open banner selecting {clip} — playback unproven"
    try:
        yield int(m.group(1))  # the clip's format id, for format assertions
    finally:
        rgb.exec("anim set zigzag", check=False)
        if orig_mode is not None:
            rgb.exec(f"glim set_loop_mode {orig_mode}", check=False)
        else:
            rgb.exec("glim set_loop_mode loop_one", check=False)  # firmware default
        if orig_glim:
            with contextlib.suppress(Exception):
                rgb.glim_select_name(orig_glim)


def _collect_console(rgb: RgbShell, seconds: float) -> list[str]:
    """Drain console lines for a window, prompt-stripped (the shell redraw
    prefixes every async log line)."""
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
    """#267/#271/#312: 5 minutes under PROVEN rendering load (LZ4 decode +
    FAT reads + render), then the led_stats budget gates."""
    _require_shuffle_off(rgb)
    _require_default_divider(rgb)
    with _glim_playback(rgb, GLIM_LZ4):
        rgb.exec("led_stats reset")
        console = _collect_console(rgb, 300.0)

        errors = [ln for ln in console if "<err>" in ln or "Failed to allocate" in ln]
        assert not errors, f"error lines during the soak: {errors[:5]}"

        s = rgb.led_stats()
        # The frame period is a persisted, app-writable setting — derive the
        # expected count from the device's own target instead of assuming
        # 33.3 ms (PR #349 review).
        expected = 300e6 / s["target_us"]
        assert s["frames"] > expected * 0.9, (
            f"only {s['frames']} frames in 300 s (expected ~{expected:.0f} "
            f"at {s['target_us']} µs/frame): {s}"
        )
        assert s["overruns"] == 0, f"frame overruns during soak (#267): {s}"
        # The render thread is phase-locked to the display clock (#379), so at
        # the default 1:1 divider a display cycle re-shows an unchanged frame
        # only when a render genuinely overran its full display period. The
        # pre-fix free-running slip measured ~1 held frame per 60 (135 in
        # 8,243), so a 0.1% allowance still catches any regression by two
        # orders of magnitude while tolerating the occasional LZ4/FAT render
        # tick this gate has not yet been measured under (PR #381 review) —
        # tighten to == 0 once a soak pass has demonstrated it.
        held_budget = max(1, s["frames"] // 1000)
        assert s["held_frames"] <= held_budget, (
            f"held frames {s['held_frames']} > budget {held_budget} during soak (#379): {s}"
        )
        assert s["work_max_us"] < s["target_us"], (
            f"work max {s['work_max_us']} µs exceeds the {s['target_us']} µs "
            f"frame budget (#267): {s}"
        )
        assert s["worst_wall_us"] < 100_000, (
            f"segment stall {s['worst_wall_us']} µs in '{s['worst_label']}' (#312): {s}"
        )


def test_ext_cpu_soak(rgb: RgbShell):
    """#304/#307: >300 s of continuous extension ticking; CPU max must stay
    flat. The #307 collapse (3.4 ms → 25 ms cpu max) is invisible before
    ~200 s because sinf's argument has to grow first."""
    _require_shuffle_off(rgb)
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
            assert not cur["faulted"], f"{target} faulted mid-soak: {rgb.ext_faults()}"
            # Deactivation (e.g. anything switching animations) silently
            # stops the accumulator growing — the soak would then measure
            # nothing while its cumulative floors still pass (PR #349
            # review).
            assert cur["active"], (
                f"{target} deactivated mid-soak — nothing is accumulating; "
                f"the #304/#307 gate would be vacuous"
            )
        stats = rgb.ext_stats()[target]
        assert stats["ticks"] > 5000, f"implausibly few ticks after 320 s: {stats}"
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
    _require_shuffle_off(rgb)  # also: the dwell clock must start at OUR shuffle-on
    st = rgb.shuffle_status()
    # min > max is legitimate (two GATT characteristics written one at a
    # time; rearm() swaps at pick time) — the effective upper bound is
    # max(min, max) (PR #349 review).
    effective_max = max(st["min_s"], st["max_s"])
    if effective_max + st["grace_s"] > 170:
        pytest.skip(
            f"persisted shuffle timing (effective max {effective_max} s + "
            f"grace {st['grace_s']} s) overlaps the clip-end window; cannot "
            f"discriminate a #257 regression on this configuration"
        )

    with _glim_playback(rgb, GLIM_LONG):
        t0 = time.monotonic()
        rgb.exec("anim shuffle on")
        try:
            dwell = None
            while time.monotonic() - t0 < 300.0:
                if rgb.anim_get() != "glim_player":
                    dwell = time.monotonic() - t0
                    break
                time.sleep(5.0)
            assert dwell is not None, (
                "shuffle never switched away within 300 s — stuck waiting "
                "for a good moment that never signals?"
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
            # The precondition proved shuffle was OFF before we enabled it,
            # so off IS the captured original state.
            rgb.exec("anim shuffle off", check=False)


def test_glim_lz4_full_pass(rgb: RgbShell):
    """#234: one complete playthrough of the LZ4 (format 4) asset with zero
    decoder errors — the regression rendered RGB bytes as a 1-bit bitmap
    because format 4 fell into the mono branch."""
    _require_shuffle_off(rgb)
    with _glim_playback(rgb, GLIM_LZ4) as fmt:
        assert fmt == 4, f"{GLIM_LZ4} opened as format {fmt}, expected LZ4 (4)"
        console = _collect_console(rgb, GLIM_LZ4_PASS_S)
        # Scoped to the decoder's own module: an unscoped fail/error match
        # fired on unrelated subsystems (BT reconnects, charger warnings)
        # during a 2-minute window (PR #349 review).
        bad = [
            ln
            for ln in console
            if "glim_decoder" in ln
            and ("<err>" in ln or re.search(r"fail|error|invalid|corrupt", ln, re.I))
        ]
        assert not bad, f"decoder complaints during the LZ4 pass: {bad[:5]}"
        assert rgb.anim_get() == "glim_player", (
            "playback did not survive the full pass"
        )
