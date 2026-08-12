"""DFU-tier helpers: re-signing the built app at a bumped version, and
fresh MCUmgr handles that survive re-enumeration.

Mirrors zephyr/tests/boot/with_mcumgr/pytest/west_sign_wrapper.py, adapted
for this sysbuild tree: the signing key is the sysbuild-generated
GENERATED_NON_SECURE_SIGN_KEY_PRIVATE.pem at the build root, and `west sign
-t imgtool --build-dir <app build>` pulls header/align/slot parameters from
the build itself, so nothing here hardcodes flash geometry.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from twister_harness.helpers.mcumgr import MCUmgr

from helpers import ports


def _imgtool_path() -> Path:
    zephyr_base = os.environ.get("ZEPHYR_BASE", "/root/ncs/v3.1.1/zephyr")
    return Path(zephyr_base).parent / "bootloader" / "mcuboot" / "scripts" / "imgtool.py"


def _sysbuild_sign_args(app_build_dir: str) -> list[str]:
    """The imgtool arguments THE BUILD ITSELF used, from its CMakeCache
    (`imgtool_sign_sysbuild:STRING=--slot-size;0xdc000;--pad-header;...`).

    `west sign`'s own derivation is wrong on this NCS sysbuild tree (it
    computed --header-size 0, hardware-observed) — partition-manager owns
    the offsets, so the cache line is the only trustworthy source and it
    tracks the build instead of hardcoding flash geometry here.
    """
    cache = Path(app_build_dir) / "CMakeCache.txt"
    for line in cache.read_text().splitlines():
        if line.startswith("imgtool_sign_sysbuild:STRING="):
            return line.split("=", 1)[1].split(";")
    raise AssertionError(f"imgtool_sign_sysbuild not found in {cache}")


def create_bumped_image(build_dir: str, app_build_dir: str, version: str) -> Path:
    """Re-sign the already-built app at `version`; returns the .bin path.

    A different version changes the image header, hence the SHA — which is
    the whole verification story on this board (image versions all report
    0.0.0; the hash is the identity).
    """
    build = Path(build_dir)
    # The key MCUboot actually verifies against is CONFIG_BOOT_SIGNATURE_KEY_FILE
    # from the MCUboot image's own config — NOT the GENERATED_NON_SECURE_*
    # pem at the build root (that one is a different subsystem's key and
    # produces an ECDSA signature the bootloader would reject;
    # hardware/byte-diff-observed: build TLVs are RSA2048).
    mcuboot_cfg = build / "mcuboot" / "zephyr" / ".config"
    key = None
    for line in mcuboot_cfg.read_text().splitlines():
        if line.startswith("CONFIG_BOOT_SIGNATURE_KEY_FILE="):
            key = Path(line.split("=", 1)[1].strip().strip('"'))
            break
    assert key and key.exists(), f"MCUboot signing key not resolved from {mcuboot_cfg}"
    unsigned = Path(app_build_dir) / "zephyr" / "zephyr.bin"
    assert unsigned.exists(), f"unsigned app image not found: {unsigned}"
    out = build / f"dfu_test_{version.replace('+', '_')}.signed.bin"

    cmd = [
        "python3", str(_imgtool_path()), "sign",
        "--key", str(key),
        "--align", "4",
        "--version", version,
        *_sysbuild_sign_args(app_build_dir),
        str(unsigned), str(out),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, (
        f"imgtool sign failed ({result.returncode}):\n{result.stdout}\n{result.stderr}"
    )
    assert out.exists(), f"imgtool sign succeeded but {out} missing"
    return out


def fresh_mcumgr() -> MCUmgr:
    """An MCUmgr handle on the CURRENT SMP port.

    Never cache one across a reboot: the CDC functions re-enumerate under
    new ttyACM minors (the same reason tty-bridge.py exists for the shell
    side), so a session-scoped handle points at a dead node after the DFU
    reset."""
    port = ports.find_smp_port()
    assert port, "board SMP port (2fe3:0001 interface 02) not found"
    return MCUmgr.create_for_serial(port)
