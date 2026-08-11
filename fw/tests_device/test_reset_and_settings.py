"""Reset-cause and settings-persistence regression tests (reboot-heavy).

Regressions pinned:
- #325/#192  RESETREAS must be readable via the stock `hwinfo reset_cause`
        AND must not be cleared by anything at boot (the custom module that
        did so is exactly what #325 removed). Bits accumulate unless cleared
        by the user — the clear→reboot→exactly-software cycle checks both.
- #311/#313  An animation switch must NOT persist anything (flash endurance);
        the device always boots to the default animation (zigzag).
- #114/#118  The persistent-value registry has no fixed cap: values from two
        DIFFERENT persistence consumers (glim's bespoke by-name path, an
        extension's param blob) round-trip a warm reboot, with no -ENOMEM.
"""

from __future__ import annotations

import time

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.integration

DEFAULT_ANIMATION = "zigzag"  # pattern_controller.cpp boot-time switch


def _reset_causes(rgb: RgbShell) -> set[str]:
    causes = set()
    for line in rgb.exec("hwinfo reset_cause show"):
        s = line.strip()
        if s.startswith("- "):
            causes.add(s[2:].strip())
    return causes


def test_reset_cause_software_no_accumulation(rgb: RgbShell):
    # Never assert this via the boot banner: the USB CDC console only comes up
    # ~8 s into boot, long after the early boot log has scrolled away.
    for round_no in range(2):
        rgb.exec("hwinfo reset_cause clear")
        rgb.reboot(cold=True)
        causes = _reset_causes(rgb)
        assert causes == {"software"}, (
            f"round {round_no}: expected exactly {{'software'}} after "
            f"clear + `kernel reboot cold`, got {causes or 'nothing (cleared at boot? #325)'}"
        )


def test_anim_not_persisted(rgb: RgbShell):
    rgb.exec("anim set rainbow")
    assert rgb.anim_get() == "rainbow"

    # The switch itself must not schedule a settings write (issue #311 removed
    # it; the debounce is well under this). No key naming the active animation
    # may exist — earlier firmware's inert leftovers were named
    # appcfg/core/last_active_*.
    time.sleep(3.0)
    anim_keys = [k for k in rgb.settings_keys() if "last_active" in k.lower()]
    assert not anim_keys, f"active-animation key(s) present in settings: {anim_keys}"

    rgb.reboot()
    assert rgb.anim_get() == DEFAULT_ANIMATION, (
        "animation selection survived a reboot — the #311/#313 "
        "no-persist-on-switch contract is broken"
    )


@pytest.mark.requires_provisioned
@pytest.mark.requires_ext("hello")
def test_settings_roundtrip(rgb: RgbShell):
    glim_before = rgb.glim_list()
    assert len(glim_before) >= 2, f"need ≥2 glim files: {glim_before}"

    hello = next(s for s in rgb.ext_list() if s["name"] == "hello")
    slot = hello["slot"]

    # hello param 0 is Speed (UINT32, default 50) — see fw/extensions/hello.
    # NEVER write params 2/3 here: those are the Crash/Hang fault injectors.
    def read_speed(slot_no: int) -> int:
        out = rgb.exec(f"ext param {slot_no} 0")
        kv = RgbShell.parse_kv([line.replace(" = ", "=") for line in out])
        vals = [v for k, v in kv.items() if k.lower().endswith("speed")]
        assert vals, f"could not read hello.Speed: {out}"
        return vals[0]

    orig_speed = read_speed(slot)
    new_speed = 77 if orig_speed != 77 else 78
    orig_glim_idx = 0  # restored at the end; selection is persisted by name

    try:
        rgb.exec("glim select 1")
        rgb.exec(f"ext param {slot} 0 {new_speed}")
        # Persistence is debounced; give the store one flush window.
        time.sleep(3.0)

        boot_log = rgb.reboot()
        assert not any("-ENOMEM" in line for line in boot_log), (
            f"persistent-value registry overflow in boot log (#114/#118): "
            f"{[line for line in boot_log if 'ENOMEM' in line]}"
        )

        glim_after = rgb.exec("glim get_selected")
        assert any(glim_before[1] in line for line in glim_after), (
            f"glim selection did not survive reboot: expected "
            f"{glim_before[1]!r}, got {glim_after}"
        )
        # Slots can renumber across boots; the extension's NAME is its
        # identity (that's the #303 lesson) — re-resolve before reading.
        hello_after = next(s for s in rgb.ext_list() if s["name"] == "hello")
        assert read_speed(hello_after["slot"]) == new_speed, (
            "extension param blob did not survive reboot"
        )
    finally:
        # Restore defaults so the suite leaves the board as it found it.
        hello_now = next(s for s in rgb.ext_list() if s["name"] == "hello")
        rgb.exec(f"ext param {hello_now['slot']} 0 {orig_speed}")
        rgb.exec(f"glim select {orig_glim_idx}")
        time.sleep(3.0)  # let the restore flush before any later reboot
