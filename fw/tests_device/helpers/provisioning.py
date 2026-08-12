"""Baseline provisioning manifest for the on-device suite.

The canonical asset set comes from fw/scripts/provision-device.sh (the same
list documented in fw/CLAUDE.md, "Setting up GLIM files on a new board").
Tests never provision the board themselves in the smoke/integration tiers —
the session fixture verifies the baseline and fails fast with instructions.
The destructive tier's reprovision teardown lands with that tier (PR 2).
"""

from __future__ import annotations

import contextlib
import glob
import os
import subprocess
import tempfile

# .glim files provision-device.sh generates into /NAND:/glim.
EXPECTED_GLIM = {"nyan_cat.glim", "bad_apple.glim", "4096.glim"}

# In-repo extensions (fw/extensions/*/) built + copied into /NAND:/ext,
# identified by manifest displayName (what `ext list` prints), NOT filename.
# Hello is the fault-injection workhorse (Crash/Hang params).
EXPECTED_EXT = {"Hello Extension"}


def _find_nand_disk() -> str | None:
    """The board's USB MSC disk, identified by SCSI strings — never /dev/sdX
    position (same discovery as provision-device.sh)."""
    for dev in glob.glob("/sys/block/sd*"):
        try:
            with open(os.path.join(dev, "device", "vendor")) as f:
                vendor = f.read().strip()
            with open(os.path.join(dev, "device", "model")) as f:
                model = f.read().strip()
        except OSError:
            continue
        if vendor == "RGB-SG" and model == "FlashDisk":
            return "/dev/" + os.path.basename(dev)
    return None


@contextlib.contextmanager
def nand_mount():
    """Mount the board's NAND over USB MSC. FAT-coherence contract: the
    firmware caches cluster allocations, so after any host-side write the
    caller MUST reboot the board before firmware reads the files
    (fw/CLAUDE.md, 'FAT concurrent access causes read corruption')."""
    disk = _find_nand_disk()
    assert disk, "NAND USB mass-storage disk not found (vendor=RGB-SG model=FlashDisk)"
    mnt = tempfile.mkdtemp(prefix="hil-nand-")
    subprocess.run(["mount", "-o", "rw", disk, mnt], check=True, timeout=30)
    try:
        yield mnt
    finally:
        subprocess.run(["sync"], timeout=60)
        # check=True is load-bearing: a swallowed umount failure leaves the
        # volume mounted rw with dirty blocks while the firmware remounts
        # FAT — the double-writer corruption this docstring warns about —
        # and the unconditional rmdir then raised EBUSY from the finally,
        # REPLACING the body's real exception (PR #348 review).
        subprocess.run(["umount", mnt], check=True, timeout=60)
        os.rmdir(mnt)


def plant_corrupt_extension(name: str = "zz_bad.llext", source: str = "hello.llext") -> None:
    """Write a deliberately-corrupted .llext into /NAND:/ext (the #89 case:
    an untrusted file that must be rejected, not deref'd, at boot).

    Takes a REAL installed extension from the board itself (no build-path
    coupling) and corrupts it STRUCTURALLY: the ELF magic stays intact (so
    rejection isn't the trivial magic check) but e_shoff — the section
    header table offset — is pointed far past EOF. Every loader walk of the
    section table is then an out-of-bounds access it must refuse. This is
    deterministic, unlike scrambling a byte range, whose effect depended on
    the section-size ratio of whatever hello.llext was built (PR #348
    review: a mostly-.text scramble could load cleanly and false-alarm the
    test). Caller must reboot afterwards (FAT-coherence contract).
    """
    with nand_mount() as mnt:
        src = os.path.join(mnt, "ext", source)
        with open(src, "rb") as f:
            data = bytearray(f.read())
        assert len(data) > 512, f"{source} implausibly small ({len(data)} B)"
        assert data[:4] == b"\x7fELF" and data[4] == 1, "expected ELF32"
        # ELF32: e_shoff is a 4-byte LE word at offset 0x20.
        data[0x20:0x24] = (0xFFFFFF00).to_bytes(4, "little")
        with open(os.path.join(mnt, "ext", name), "wb") as f:
            f.write(bytes(data))


def hard_reset(jlink_serial: str) -> None:
    """Reset the target over the J-Link's SWD connection — works even when
    the firmware is halted and the shell is gone. Never touches flash
    (fw/CLAUDE.md, 'Recovering a wedged shell UART without reflashing')."""
    subprocess.run(
        ["nrfutil", "device", "reset",
         "--serial-number", str(jlink_serial), "--reset-kind", "RESET_PIN"],
        check=True, timeout=60,
    )


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
