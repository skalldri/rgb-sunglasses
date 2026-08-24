#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Write the rgbx-sdk manifest that pins what produced an SDK tree.

The manifest is the SDK's provenance record and its policy carrier at once:

  * every toolchain input is named by exact version AND by the SHA-256 of the
    distribution archive the installers verify before use, per host;
  * the RGBX v2 admission profile is copied verbatim out of the ABI header, so
    the SDK's post-link gate and the firmware admission path read the same
    numbers instead of each carrying their own copy;
  * every shipped file is recorded by SHA-256, which pins the exact compiler
    profile, gate, and packager sources that produced the archive.

Output is byte-deterministic for a given tree: sorted keys, sorted file list,
no timestamps, no host or user identity.
"""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

MANIFEST_NAME = "sdk-manifest.json"

# The RGBX v2 admission profile (its field names, its capability names, and the
# ABI versions) is read by fw/sdk/tools/dump-abi-macros.sh, which evaluates the
# ABI header with the C compiler. That is the single source both this packager
# and the drift gate consume, and it is immune to the #if 0 / commented-decoy
# spoof that first-match text scraping falls for.

# Distribution-archive digests the installers verify before unpacking. Host key
# -> shell variable in the installer that owns the pin.
WASI_SDK_DIGESTS = {
    "x86_64-linux": "WASI_SDK_SHA256_X86_64_LINUX",
    "arm64-linux": "WASI_SDK_SHA256_ARM64_LINUX",
    "x86_64-macos": "WASI_SDK_SHA256_X86_64_MACOS",
    "arm64-macos": "WASI_SDK_SHA256_ARM64_MACOS",
}
ARM_TOOLCHAIN_DIGESTS = {
    "x86_64-linux": "ARM_TOOLCHAIN_SHA256_X86_64_LINUX",
    "aarch64-linux": "ARM_TOOLCHAIN_SHA256_AARCH64_LINUX",
    "x86_64-macos": "ARM_TOOLCHAIN_SHA256_DARWIN_X86_64",
    "arm64-macos": "ARM_TOOLCHAIN_SHA256_DARWIN_ARM64",
}


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


def read_text(path: Path) -> str:
    if not path.is_file():
        fail(f"missing provenance input: {path}")
    return path.read_text(encoding="utf-8")


def abi_constants(repo_root: Path) -> dict:
    """Evaluate the RGBX ABI constants with the compiler-backed extractor.

    Returns {"abi": {...}, "cap": {name: bit}, "policy": {field: value}} with
    integer values. The extractor fails closed on a redefined or absent macro.
    """
    script = repo_root / "fw/sdk/tools/dump-abi-macros.sh"
    include_dir = repo_root / "fw/include"
    result = subprocess.run(
        ["bash", str(script), str(include_dir)],
        check=True, capture_output=True, text=True)
    out: dict = {"abi": {}, "cap": {}, "policy": {}}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[0] not in out:
            fail(f"unexpected dump-abi-macros line: {line!r}")
        out[parts[0]][parts[1]] = int(parts[2])
    for section in ("abi", "cap", "policy"):
        if not out[section]:
            fail(f"dump-abi-macros produced no {section} constants")
    return out


def shell_pin(source: str, name: str, path: Path) -> str:
    match = re.search(rf'^{re.escape(name)}="([0-9A-Za-z._-]+)"\s*$', source, re.MULTILINE)
    if match is None:
        fail(f"{name} is not a single-line pin in {path}")
    return match.group(1)


def shell_digests(source: str, mapping: dict, path: Path) -> dict:
    digests = {}
    for host, variable in sorted(mapping.items()):
        value = shell_pin(source, variable, path)
        if not re.fullmatch(r"[0-9a-f]{64}", value):
            fail(f"{variable} in {path} is not a lowercase SHA-256 digest")
        digests[host] = value
    return digests


def sdk_file_digests(root: Path) -> dict:
    digests = {}
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        # Check the symlink first: a symlink to a directory answers is_dir()
        # (it follows the link), so a plain is_dir() skip would drop it silently
        # from the provenance record instead of rejecting it.
        if path.is_symlink():
            fail(f"symlink in SDK tree is not allowed: {relative}")
        if path.is_dir():
            continue
        if relative == MANIFEST_NAME:
            continue
        if not path.is_file():
            fail(f"unsupported SDK tree entry: {relative}")
        digests[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    if not digests:
        fail("the assembled SDK tree is empty")
    return digests


def main() -> None:
    if len(sys.argv) != 4:
        fail("usage: write-sdk-manifest.py <repo-root> <sdk-root> <version>")
    repo_root = Path(sys.argv[1]).resolve()
    sdk_root = Path(sys.argv[2]).resolve()
    version = sys.argv[3]
    if not sdk_root.is_dir():
        fail("sdk-root must be the assembled SDK directory")

    abi = abi_constants(repo_root)
    wasi_installer_path = repo_root / "fw/sim/scripts/install-toolchain.sh"
    arm_installer_path = repo_root / "fw/sdk/scripts/install-arm-toolchain.sh"
    wasi_installer = read_text(wasi_installer_path)
    arm_installer = read_text(arm_installer_path)
    board_conf = read_text(
        repo_root / "fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf")

    heap_match = re.search(r"^CONFIG_LLEXT_HEAP_SIZE=([0-9]+)\s*$", board_conf, re.MULTILINE)
    if heap_match is None:
        fail("CONFIG_LLEXT_HEAP_SIZE is not set in the proto0 board configuration")

    policy = dict(sorted(abi["policy"].items()))
    policy["capabilityBits"] = dict(sorted(abi["cap"].items()))
    if policy["sectionRequiredMask"] & ~policy["sectionAllowedMask"]:
        fail("a required section id is not in the admitted section set")

    manifest = {
        "sdkVersion": version,
        "fwRelease": f"fw-v{version}",
        "abiVersion": abi["abi"]["abiVersion"],
        "rgbxV2AbiVersion": abi["abi"]["rgbxV2AbiVersion"],
        "rgbxV2ModulePolicy": policy,
        "armToolchain": f"arm-gnu-{shell_pin(arm_installer, 'ARM_TOOLCHAIN_VERSION', arm_installer_path)}",
        "armToolchainSha256": shell_digests(arm_installer, ARM_TOOLCHAIN_DIGESTS,
                                            arm_installer_path),
        "wasiSdk": shell_pin(wasi_installer, "WASI_SDK_VERSION", wasi_installer_path),
        "wasiSdkSha256": shell_digests(wasi_installer, WASI_SDK_DIGESTS, wasi_installer_path),
        "llextHeapBytes": int(heap_match.group(1)) * 1024,
        "sdkFiles": sdk_file_digests(sdk_root),
    }

    (sdk_root / MANIFEST_NAME).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


main()
