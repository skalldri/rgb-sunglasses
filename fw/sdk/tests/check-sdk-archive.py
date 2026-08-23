#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import gzip
from pathlib import Path
import sys
import tarfile


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


if len(sys.argv) != 2:
    fail("usage: check-sdk-archive.py <sdk.tar.gz>")

archive_path = Path(sys.argv[1])
with tarfile.open(archive_path, "r:gz") as archive:
    members = archive.getmembers()
    names = [member.name for member in members]
    if names != sorted(names):
        fail("archive entries are not in lexical order")
    if not any(name.endswith("/LICENSE.Apache-2.0") for name in names):
        fail("archive omits LICENSE.Apache-2.0")
    for member in members:
        if Path(member.name).name.startswith("._"):
            fail(f"AppleDouble entry is forbidden: {member.name}")
        if member.uid != 0 or member.gid != 0 or member.uname or member.gname:
            fail(f"identity-bearing owner metadata on {member.name}")
        if member.mtime != 0 or member.pax_headers:
            fail(f"nondeterministic timestamp or PAX metadata on {member.name}")
        expected_mode = 0o755 if member.isdir() or (member.mode & 0o111) else 0o644
        if member.mode != expected_mode:
            fail(f"noncanonical mode {oct(member.mode)} on {member.name}")

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
