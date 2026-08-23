#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import gzip
import os
from pathlib import Path
import stat
import sys
import tarfile


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


if len(sys.argv) != 4:
    fail("usage: create-reproducible-archive.py <root-dir> <output.tar.gz> <epoch>")

root = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
try:
    epoch = int(sys.argv[3])
except ValueError:
    fail("epoch must be an integer")
if not root.is_dir() or epoch < 0:
    fail("root must be a directory and epoch must be nonnegative")

paths = [root, *sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix())]
for path in paths:
    if path.name.startswith("._"):
        fail(f"AppleDouble entry is forbidden: {path}")
    if path.is_symlink() or not (path.is_dir() or path.is_file()):
        fail(f"unsupported archive entry type: {path}")

output.parent.mkdir(parents=True, exist_ok=True)
with output.open("wb") as raw:
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for path in paths:
                arcname = Path(root.name)
                if path != root:
                    arcname /= path.relative_to(root)
                info = archive.gettarinfo(str(path), arcname.as_posix())
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                info.mtime = epoch
                info.pax_headers = {}
                if path.is_dir():
                    info.mode = 0o755
                    archive.addfile(info)
                else:
                    executable = bool(path.stat().st_mode & stat.S_IXUSR)
                    info.mode = 0o755 if executable else 0o644
                    with path.open("rb") as source:
                        archive.addfile(info, source)
