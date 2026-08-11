"""Baseline provisioning manifest for the on-device suite.

The canonical asset set comes from fw/scripts/provision-device.sh (the same
list documented in fw/CLAUDE.md, "Setting up GLIM files on a new board").
Tests never provision the board themselves in the smoke/integration tiers —
the session fixture verifies the baseline and fails fast with instructions.
The destructive tier's reprovision teardown lands with that tier (PR 2).
"""

from __future__ import annotations

import os

# .glim files provision-device.sh generates into /NAND:/glim.
EXPECTED_GLIM = {"nyan_cat.glim", "bad_apple.glim", "4096.glim"}

# In-repo extensions (fw/extensions/*/) built + copied into /NAND:/ext,
# identified by manifest displayName (what `ext list` prints), NOT filename.
# Hello is the fault-injection workhorse (Crash/Hang params).
EXPECTED_EXT = {"Hello Extension"}


def reprovision(rgb, build_dir: str) -> None:
    """Re-provision the board's NAND after a destructive test, then verify.

    Runs fw/scripts/provision-device.sh (regenerates the GLIM assets —
    downloads source videos, ~2-3 min — builds extensions, copies both over
    USB mass storage), reboots so the firmware re-mounts FAT and rescans,
    and re-checks the baseline. The script self-gates on the `board` hw-lock
    when CLAUDECODE is set, which the suite's runner contract already
    requires, and needs a configured sysbuild dir for the llext EDK.
    """
    import subprocess

    repo_root = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..")
    )
    script = os.path.join(repo_root, "fw", "scripts", "provision-device.sh")
    result = subprocess.run(
        [script, "--build-dir", build_dir],
        capture_output=True, text=True, timeout=600,
    )
    assert result.returncode == 0, (
        f"provision-device.sh failed ({result.returncode}):\n"
        f"{result.stdout[-2000:]}\n{result.stderr[-2000:]}"
    )
    rgb.reboot()  # firmware must re-mount FAT to see the new files
    problems = check_baseline(rgb.glim_list(), rgb.ext_list())
    assert not problems, f"board still unprovisioned after reprovision: {problems}"


def check_baseline(glim_names: list[str], ext_slots: list[dict]) -> list[str]:
    """Return a list of human-readable deficiencies (empty = provisioned)."""
    problems: list[str] = []
    missing_glim = EXPECTED_GLIM - set(glim_names)
    if missing_glim:
        problems.append(f"missing GLIM assets: {sorted(missing_glim)}")
    ext_names = {s["name"] for s in ext_slots}
    missing_ext = EXPECTED_EXT - ext_names
    if missing_ext:
        problems.append(f"missing extensions: {sorted(missing_ext)}")
    return problems
