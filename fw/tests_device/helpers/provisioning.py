"""Baseline provisioning manifest for the on-device suite.

The canonical asset set comes from fw/scripts/provision-device.sh (the same
list documented in fw/CLAUDE.md, "Setting up GLIM files on a new board").
Tests never provision the board themselves in the smoke/integration tiers —
the session fixture verifies the baseline and fails fast with instructions.
The destructive tier's reprovision teardown lands with that tier (PR 2).
"""

from __future__ import annotations

# .glim files provision-device.sh generates into /NAND:/glim.
EXPECTED_GLIM = {"nyan_cat.glim", "bad_apple.glim", "4096.glim"}

# In-repo extensions (fw/extensions/*/) built + copied into /NAND:/ext.
# 'hello' is the fault-injection workhorse (Crash/Hang params).
EXPECTED_EXT = {"hello"}


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
