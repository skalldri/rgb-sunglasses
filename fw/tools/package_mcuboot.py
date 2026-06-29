#!/usr/bin/env python3
"""
Package a raw MCUboot zephyr.bin into the custom 16-byte header format
expected by the BLE bootloader updater service (mcuboot_updater.cpp).

Header format (16 bytes, all fields little-endian):
  [0..3]   magic        = 0x424D5247 ("GRMB")
  [4]      version_major
  [5]      version_minor
  [6..7]   version_revision (u16 LE)
  [8..11]  payload_size (u32 LE) — byte count of raw binary
  [12..15] crc32        (u32 LE) — IEEE 802.3 CRC32 over payload bytes only
  [16+]    raw MCUboot zephyr.bin

The CRC32 uses the same algorithm as zlib.crc32() and Zephyr's crc32_ieee()
(IEEE 802.3 polynomial 0xEDB88320).

Usage:
  python3 fw/tools/package_mcuboot.py \\
      --input  fw/build/mcuboot/zephyr/zephyr.bin \\
      --output mcuboot-1.0.0-proto0.bin \\
      --major 1 --minor 0 --revision 0

CI usage (reads version from build artefact):
  VER_H=fw/build/mcuboot/zephyr/include/generated/version.h
  MAJOR=$(grep 'KERNEL_VERSION_MAJOR' $VER_H | awk '{print $3}')
  MINOR=$(grep 'KERNEL_VERSION_MINOR' $VER_H | awk '{print $3}')
  PATCH=$(grep 'KERNEL_PATCHLEVEL'    $VER_H | awk '{print $3}')
  python3 fw/tools/package_mcuboot.py \\
      --input fw/build/mcuboot/zephyr/zephyr.bin \\
      --output mcuboot-${MAJOR}.${MINOR}.${PATCH}-proto0.bin \\
      --major $MAJOR --minor $MINOR --revision $PATCH
"""

import argparse
import struct
import zlib
from pathlib import Path

MAGIC = 0x424D5247  # "GRMB" stored as little-endian uint32
HEADER_SIZE = 16


def compute_crc32(data: bytes) -> int:
    """IEEE 802.3 CRC32 — identical to Zephyr's crc32_ieee() and zlib.crc32()."""
    return zlib.crc32(data) & 0xFFFFFFFF


def pack_header(major: int, minor: int, revision: int, payload: bytes) -> bytes:
    crc = compute_crc32(payload)
    # Format: magic(I), major(B), minor(B), revision(H), payload_size(I), crc32(I)
    header = struct.pack("<IBBHII", MAGIC, major, minor, revision, len(payload), crc)
    assert len(header) == HEADER_SIZE, f"Header is {len(header)} bytes, expected {HEADER_SIZE}"
    return header


def verify_package(data: bytes) -> dict:
    """Parse and verify a packaged MCUboot binary. Returns parsed fields or raises."""
    if len(data) < HEADER_SIZE:
        raise ValueError(f"File too small: {len(data)} bytes (minimum {HEADER_SIZE})")

    magic, major, minor, revision, payload_size, stored_crc = struct.unpack_from("<IBBHII", data)

    if magic != MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:08X} (expected 0x{MAGIC:08X})")

    if len(data) != HEADER_SIZE + payload_size:
        raise ValueError(
            f"Size mismatch: header says {payload_size} payload bytes, "
            f"file has {len(data) - HEADER_SIZE}"
        )

    payload = data[HEADER_SIZE:]
    computed_crc = compute_crc32(payload)
    if computed_crc != stored_crc:
        raise ValueError(
            f"CRC32 mismatch: stored=0x{stored_crc:08X}, computed=0x{computed_crc:08X}"
        )

    return {
        "major": major,
        "minor": minor,
        "revision": revision,
        "payload_size": payload_size,
        "crc32": stored_crc,
    }


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Package a raw MCUboot zephyr.bin with a 16-byte validation header."
    )
    ap.add_argument("--input",    required=True,  help="Path to mcuboot/zephyr/zephyr.bin")
    ap.add_argument("--output",   required=True,  help="Output .bin path")
    ap.add_argument("--major",    required=True,  type=int, help="Version major")
    ap.add_argument("--minor",    required=True,  type=int, help="Version minor")
    ap.add_argument("--revision", required=True,  type=int, help="Version revision/patch")
    ap.add_argument("--verify",   action="store_true",
                    help="Verify an existing package instead of creating one")
    args = ap.parse_args()

    if args.verify:
        data = Path(args.input).read_bytes()
        info = verify_package(data)
        print(f"Valid MCUboot package: {args.input}")
        print(f"  Version:      {info['major']}.{info['minor']}.{info['revision']}")
        print(f"  Payload size: {info['payload_size']} bytes")
        print(f"  CRC32:        0x{info['crc32']:08X}")
        return

    payload = Path(args.input).read_bytes()
    header  = pack_header(args.major, args.minor, args.revision, payload)
    crc     = compute_crc32(payload)

    Path(args.output).write_bytes(header + payload)
    print(f"Written: {args.output}")
    print(f"  Version:      {args.major}.{args.minor}.{args.revision}")
    print(f"  Payload size: {len(payload)} bytes")
    print(f"  CRC32:        0x{crc:08X}")


if __name__ == "__main__":
    main()
