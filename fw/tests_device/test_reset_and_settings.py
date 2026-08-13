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
    # NOTE: inert appcfg/core/last_active_* keys written by OLD firmware may
    # legitimately be resident (deliberately never deleted — the delete would
    # itself be an NVS write; see pattern_controller.cpp). The contract is
    # that a switch creates no NEW key — hence before/after comparison, not
    # key-name matching (hardware-observed: asserting on the name failed on a
    # board upgraded from pre-#311 firmware).
    keys_before = set(rgb.settings_keys())

    rgb.exec("anim set rainbow")
    assert rgb.anim_get() == "rainbow"

    # The switch must not schedule a settings write (issue #311 removed it).
    # Wait a full debounce window, then confirm no new key appeared.
    rgb.wait_persist_flush()
    new_keys = set(rgb.settings_keys()) - keys_before
    assert not new_keys, f"animation switch created settings key(s): {new_keys}"

    rgb.reboot()
    assert rgb.anim_get() == DEFAULT_ANIMATION, (
        "animation selection survived a reboot — the #311/#313 "
        "no-persist-on-switch contract is broken"
    )


HELLO = "Hello Extension"  # ext list reports manifest displayNames, not filenames


def _find_ext(rgb: RgbShell, name: str) -> dict:
    slots = rgb.ext_list()
    matches = [s for s in slots if s["name"] == name]
    assert matches, f"extension {name!r} not loaded: {[s['name'] for s in slots]}"
    return matches[0]


@pytest.mark.requires_provisioned
@pytest.mark.requires_ext(HELLO)
def test_settings_roundtrip(rgb: RgbShell):
    glim_before = rgb.glim_list()
    assert len(glim_before) >= 2, f"need ≥2 glim files: {glim_before}"

    slot = _find_ext(rgb, HELLO)["slot"]

    # hello param 0 is Speed (UINT32, default 50) — see fw/extensions/hello.
    # NEVER write params 2/3 here: those are the Crash/Hang fault injectors.
    def read_speed(slot_no: int) -> int:
        return rgb.ext_param_int(slot_no, 0, name="Speed")

    orig_speed = read_speed(slot)
    new_speed = 77 if orig_speed != 77 else 78
    # Capture the ACTUAL current selection so the teardown restores what the
    # operator left, not a hardcoded index (PR #341 review: restoring a value
    # the test never read silently drifts shared-board state and burns an NVS
    # write on the wrong name). Selection identity is the NAME; indices are
    # per-boot.
    orig_glim_name = rgb.glim_selected()
    candidates = [n for n in glim_before if n != orig_glim_name]
    assert candidates, f"no glim file other than the selected one: {glim_before}"
    target_glim = candidates[0]

    try:
        rgb.glim_select_name(target_glim)
        rgb.exec(f"ext param {slot} 0 {new_speed}")
        rgb.wait_persist_flush()  # let the debounced store flush before reboot

        rgb.reboot()
        # NOTE deliberately NOT asserting on boot-log contents here: the CDC
        # console enumerates ~8 s into boot, so a registry -ENOMEM (#114/#118)
        # can never reach the host capture — a green assertion on it would be
        # fake coverage. Pinning that regression needs an on-device
        # registry-count readout (tracked in #334's destructive-tier issue).

        assert rgb.glim_selected() == target_glim, (
            f"glim selection did not survive reboot: expected {target_glim!r}, "
            f"got {rgb.glim_selected()!r}"
        )
        # Slots can renumber across boots; the extension's NAME is its
        # identity (that's the #303 lesson) — re-resolve before reading.
        assert read_speed(_find_ext(rgb, HELLO)["slot"]) == new_speed, (
            "extension param blob did not survive reboot"
        )
    finally:
        # Restore what was actually there so the suite leaves the board as it
        # found it (by name — indices may have shifted across the reboot).
        rgb.exec(f"ext param {_find_ext(rgb, HELLO)['slot']} 0 {orig_speed}")
        if orig_glim_name is not None:
            rgb.glim_select_name(orig_glim_name)
        rgb.wait_persist_flush()  # let the restore flush before any later reboot
