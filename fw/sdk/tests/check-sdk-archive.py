#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import gzip
import hashlib
import json
import re
from pathlib import Path
import sys
import tarfile


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


# Files that carry no SPDX identifier, by format. Each is either a license-text
# file (whose content is the license itself) or a generated file whose format
# has no comment syntax:
#   LICENSE            the MIT permission notice
#   NOTICE             the attribution notice for the one Apache-2.0 component
#   sdk-manifest.json  generated JSON; the format has no comment syntax
#   arm/heap-limit.txt a single generated integer
UNLICENSED_BY_FORMAT = frozenset({
    "LICENSE",
    "NOTICE",
    "sdk-manifest.json",
    "arm/heap-limit.txt",
})

# The SDK ships under MIT, with one Zephyr-derived file that stays Apache-2.0.
# Expected SPDX identifiers are policy, not read back from the file: a shipped
# file whose header disagrees with the license it is supposed to carry is
# rejected, in either direction.
APACHE_FILES = frozenset({
    "arm/shim/include/zephyr/llext/symbol.h",
})
DEFAULT_SPDX = "MIT"

# The only files that ship executable. Expected file modes are derived from this
# policy, not from the mode the archive happens to carry: reading the executable
# bit back off the member and calling it "expected" is self-approving, so a data
# file marked executable would pass. package-sdk.sh chmods exactly these.
EXECUTABLE_FILES = frozenset({
    "scripts/install-arm-toolchain.sh",
    "scripts/install-wasi-sdk.sh",
    "arm/check-llext.sh",
})

if len(sys.argv) != 2:
    fail("usage: check-sdk-archive.py <sdk.tar.gz>")

archive_path = Path(sys.argv[1])
recorded: dict = {}
observed: dict = {}
with tarfile.open(archive_path, "r:gz") as archive:
    members = archive.getmembers()
    names = [member.name for member in members]
    manifest_members = [m for m in members if m.name.split("/", 1)[-1] == "sdk-manifest.json"]
    if len(manifest_members) != 1:
        fail("archive must contain exactly one sdk-manifest.json")
    manifest_file = archive.extractfile(manifest_members[0])
    if manifest_file is None:
        fail("sdk-manifest.json is not a regular file")
    recorded = json.loads(manifest_file.read()).get("sdkFiles", {})
    if names != sorted(names):
        fail("archive entries are not in lexical order")
    if len(names) != len(set(names)):
        duplicates = sorted({name for name in names if names.count(name) > 1})
        fail(f"archive contains duplicate member names: {duplicates}")
    relatives = {name.split("/", 1)[1] for name in names if "/" in name}
    for required in ("LICENSE", "NOTICE"):
        if required not in relatives:
            fail(f"archive omits {required}")
    for member in members:
        if Path(member.name).name.startswith("._"):
            fail(f"AppleDouble entry is forbidden: {member.name}")
        if not (member.isfile() or member.isdir()):
            fail(f"unsupported archive entry type on {member.name}")
        if member.uid != 0 or member.gid != 0 or member.uname or member.gname:
            fail(f"identity-bearing owner metadata on {member.name}")
        if member.mtime != 0 or member.pax_headers:
            fail(f"nondeterministic timestamp or PAX metadata on {member.name}")
        relative = member.name.split("/", 1)[1] if "/" in member.name else member.name
        if member.isdir():
            expected_mode = 0o755
        elif relative in EXECUTABLE_FILES:
            expected_mode = 0o755
        else:
            expected_mode = 0o644
        if member.mode != expected_mode:
            fail(f"noncanonical mode {oct(member.mode)} on {member.name} "
                 f"(policy expects {oct(expected_mode)})")

        if member.isdir():
            continue
        content = archive.extractfile(member)
        if content is None:
            fail(f"unsupported archive entry type: {member.name}")
        payload = content.read()
        if relative != "sdk-manifest.json":
            observed[relative] = hashlib.sha256(payload).hexdigest()

        # Every shipped file states its license in-band, and it must be the
        # license that file is supposed to carry. Shipping the LICENSE and
        # NOTICE alone leaves a recipient who copies one file out of the tree
        # with nothing to go on, and a wrong identifier misrepresents the terms
        # (an original file claiming Apache-2.0, or the Zephyr-derived shim
        # claiming MIT) just as dangerously as a missing one.
        if relative in UNLICENSED_BY_FORMAT:
            continue
        match = re.search(rb"SPDX-License-Identifier:\s*(\S+)", payload)
        if match is None:
            fail(f"shipped file carries no SPDX license identifier: {member.name}")
        actual_spdx = match.group(1).decode("ascii", "replace")
        expected_spdx = "Apache-2.0" if relative in APACHE_FILES else DEFAULT_SPDX
        if actual_spdx != expected_spdx:
            fail(f"shipped file {member.name} declares SPDX {actual_spdx}, "
                 f"policy requires {expected_spdx}")

# The manifest's provenance record must cover the archive exactly. A file the
# record omits is a file whose contents nothing pins, which defeats the point
# of recording digests at all.
if observed != recorded:
    for path in sorted(set(observed) - set(recorded)):
        fail(f"shipped file is absent from the manifest's provenance record: {path}")
    for path in sorted(set(recorded) - set(observed)):
        fail(f"manifest records a file the archive does not ship: {path}")
    for path in sorted(observed):
        if observed[path] != recorded[path]:
            fail(f"manifest records a different digest than the archive ships for {path}")

with gzip.open(archive_path, "rb") as compressed:
    tar_bytes = compressed.read()
for forbidden in (
    b"com.apple.provenance",
    b"LIBARCHIVE.xattr",
    b"SCHILY.xattr",
):
    if forbidden in tar_bytes:
        fail(f"archive contains forbidden private/xattr marker {forbidden!r}")

print("SDK archive metadata and privacy checks passed")
