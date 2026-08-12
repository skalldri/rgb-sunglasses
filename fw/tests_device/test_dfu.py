"""DFU tier: a real MCUmgr firmware update over the SMP serial transport.

FORWARD-ONLY by hardware design: proto0 is MCUboot OVERWRITE_ONLY
(SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y) — there is no revert path, so the
upstream upload/test/revert pattern (zephyr/tests/boot/with_mcumgr) does not
transfer. The flow mirrors the companion app's own OTA (E2E-02): upload →
confirm the uploaded hash → reset → MCUboot copies slot 1 over slot 0
(~40 s — the same copy that bit us as the staged-OTA incident) → verify the
ACTIVE hash is the uploaded one.

Verification is hash-identity, not version strings: every image on this
board reports `version: 0.0.0` (not yet wired), so the bumped-version
re-sign exists to CHANGE THE SHA, and the SHA is what is asserted.

The teardown J-Link-reflashes the canonical twister image UNCONDITIONALLY —
on success the board is left running a junk-versioned re-sign, on failure
an unknown state; both are wrong for whatever runs next. This runs
unattended by explicit decision (PR #341 design): the J-Link is attached on
every rig this tier targets, and the reflash is the recovery path.
"""

from __future__ import annotations

import os
import subprocess
import time

import pytest

from helpers import dfu
from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.dfu

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


@pytest.fixture(autouse=True)
def restore_canonical_image(rgb: RgbShell, dut):
    yield
    # ALWAYS restore, pass or fail — see module docstring. jlink-flash.sh
    # self-gates on the board hw-lock (held for the whole tier run) and
    # flashes bootloader + app + netcore from the canonical build.
    rgb.mark_reboot_reference()
    result = subprocess.run(
        [
            os.path.join(REPO_ROOT, "fw", "scripts", "jlink-flash.sh"),
            str(dut.device_config.build_dir),
            "--",
            "--skip-rebuild",
        ],
        capture_output=True, text=True, timeout=300,
    )
    assert result.returncode == 0, (
        f"canonical-image restore flash failed ({result.returncode}):\n"
        f"{result.stdout[-1500:]}\n{result.stderr[-1500:]}"
    )
    rgb.wait_reboot(timeout=180)


def test_forward_only_update(rgb: RgbShell, dut):
    # --- baseline -------------------------------------------------------
    mgr = dfu.fresh_mcumgr()
    imgs = mgr.get_image_list()
    active = [i for i in imgs if i.image == 0 and "active" in (i.flags or "")]
    assert active, f"no active image-0 entry: {imgs}"
    old_hash = active[0].hash
    assert not any("pending" in (i.flags or "") for i in imgs), (
        f"staged image present before the test: {imgs}"
    )

    # --- build a same-source image with a DIFFERENT hash ----------------
    version = f"0.0.0+{int(time.time()) % 86400}"
    signed = dfu.create_bumped_image(
        str(dut.device_config.build_dir),
        str(dut.device_config.app_build_dir),
        version,
    )

    # --- upload over SMP serial (~3-4 min at 115200) ---------------------
    mgr.image_upload(str(signed), timeout=600)
    new_hash = dfu.fresh_mcumgr().get_hash_to_test()
    assert new_hash and new_hash != old_hash, (
        f"uploaded image hash {new_hash!r} does not differ from the running "
        f"one — the re-sign changed nothing?"
    )

    # --- confirm + reset (the app's UPGRADE_ONLY flow: no test/revert) ---
    dfu.fresh_mcumgr().image_confirm(new_hash)
    rgb.mark_reboot_reference()
    dfu.fresh_mcumgr().reset_device()
    # MCUboot's slot copy alone takes ~40 s before the app even starts.
    rgb.wait_reboot(timeout=240)

    # --- verify: the uploaded image IS the running image -----------------
    imgs = dfu.fresh_mcumgr().get_image_list()
    active = [i for i in imgs if i.image == 0 and "active" in (i.flags or "")]
    assert active and active[0].hash == new_hash, (
        f"active image-0 hash is not the uploaded image after the update: "
        f"{imgs} (expected {new_hash})"
    )
    assert "confirmed" in (active[0].flags or ""), f"not confirmed: {active[0]}"
    assert not any("pending" in (i.flags or "") for i in imgs), (
        f"stale pending image after the update: {imgs}"
    )

    # --- on-device sanity: the updated firmware actually works -----------
    assert rgb.anim_get() == "zigzag", "updated image did not boot to default"
    assert rgb.ext_list(), "updated image lost its extensions"
    # Deliberately NOT asserting a reset cause here: hardware-observed, a
    # boot that goes through MCUboot's slot copy reads RESETREAS as pin-only
    # even though the same `mcumgr reset` on a plain boot reads software
    # (probed both ways, 2026-08-12). The cause chain tests the bootloader's
    # RESETREAS side effects, not DFU correctness — freshness is already
    # proven by wait_reboot()'s uptime reference and the hash swap.
