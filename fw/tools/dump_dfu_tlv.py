#!/usr/bin/env python3
"""Dump MCUBoot image header and TLV records from all binaries in a dfu_application.zip."""

import argparse
import json
import struct
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

# ── MCUBoot constants (from bootloader/mcuboot/scripts/imgtool/image.py) ──────

IMAGE_MAGIC = 0x96F3B83D

TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_INFO_SIZE = 4  # magic(2) + tot(2)
TLV_ENTRY_HDR_SIZE = 4  # type(2) + len(2)

IMAGE_HDR_FMT = "<IIHHIIBBHII"  # 32 bytes
IMAGE_HDR_SIZE = struct.calcsize(IMAGE_HDR_FMT)  # == 32

TLV_NAMES = {
    0x01: "KEYHASH",
    0x02: "PUBKEY",
    0x10: "SHA256",
    0x11: "SHA384",
    0x12: "SHA512",
    0x20: "RSA2048",
    0x22: "ECDSASIG",
    0x23: "RSA3072",
    0x24: "ED25519",
    0x25: "SIG_PURE",
    0x30: "ENCRSA2048",
    0x31: "ENCKW",
    0x32: "ENCEC256",
    0x33: "ENCX25519",
    0x34: "ENCX25519_SHA512",
    0x40: "DEPENDENCY",
    0x50: "SEC_CNT",
    0x60: "BOOT_RECORD",
    0x70: "DECOMP_SIZE",
    0x71: "DECOMP_SHA",
    0x72: "DECOMP_SIGNATURE",
    0x73: "COMP_DEC_SIZE",
}

IMAGE_FLAGS = {
    0x00000001: "PIC",
    0x00000004: "ENCRYPTED_AES128",
    0x00000008: "ENCRYPTED_AES256",
    0x00000010: "NON_BOOTABLE",
    0x00000020: "RAM_LOAD",
    0x00000100: "ROM_FIXED",
    0x00000200: "COMPRESSED_LZMA1",
    0x00000400: "COMPRESSED_LZMA2",
    0x00000800: "COMPRESSED_ARM_THUMB",
}

SHA_TYPES = {0x10, 0x11, 0x12, 0x71}  # SHA256/384/512/DECOMP_SHA


# ── Data types ────────────────────────────────────────────────────────────────

@dataclass
class ImageVersion:
    major: int
    minor: int
    revision: int
    build: int

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.revision}+{self.build}"


@dataclass
class TlvEntry:
    type_id: int
    data: bytes

    @property
    def name(self) -> str:
        if self.type_id in TLV_NAMES:
            return TLV_NAMES[self.type_id]
        if 0x00A0 <= self.type_id <= 0xFFFE:
            return "VENDOR"
        return "UNKNOWN"


@dataclass
class ImageHeader:
    magic: int
    load_addr: int
    hdr_size: int
    protected_tlv_size: int
    img_size: int
    flags: int
    version: ImageVersion
    offset: int  # byte offset of magic within the binary


@dataclass
class ParsedImage:
    filename: str
    header: ImageHeader
    protected_tlvs: list[TlvEntry]
    unprotected_tlvs: list[TlvEntry]
    manifest_entry: Optional[dict]
    error: Optional[str] = None


# ── Parsing ───────────────────────────────────────────────────────────────────

def find_magic(data: bytes) -> int:
    """Return byte offset of IMAGE_MAGIC, searching the first 0x400 bytes. -1 if not found."""
    magic_bytes = struct.pack("<I", IMAGE_MAGIC)
    idx = data.find(magic_bytes, 0, 0x400)
    return idx


def parse_header(data: bytes, offset: int) -> ImageHeader:
    fields = struct.unpack_from(IMAGE_HDR_FMT, data, offset)
    magic, load_addr, hdr_size, protected_tlv_size, img_size, flags, \
        v_major, v_minor, v_revision, v_build, _pad = fields
    return ImageHeader(
        magic=magic,
        load_addr=load_addr,
        hdr_size=hdr_size,
        protected_tlv_size=protected_tlv_size,
        img_size=img_size,
        flags=flags,
        version=ImageVersion(v_major, v_minor, v_revision, v_build),
        offset=offset,
    )


def parse_tlv_area(data: bytes, area_start: int) -> tuple[int, list[TlvEntry]]:
    """Parse one TLV area (protected or unprotected). Returns (area_end, entries)."""
    if area_start + TLV_INFO_SIZE > len(data):
        return area_start, []

    magic, tot_size = struct.unpack_from("<HH", data, area_start)
    if magic not in (TLV_INFO_MAGIC, TLV_PROT_INFO_MAGIC):
        return area_start, []

    area_end = area_start + tot_size
    off = area_start + TLV_INFO_SIZE
    entries: list[TlvEntry] = []

    while off + TLV_ENTRY_HDR_SIZE <= area_end:
        tlv_type, tlv_len = struct.unpack_from("<HH", data, off)
        off += TLV_ENTRY_HDR_SIZE
        tlv_data = data[off: off + tlv_len]
        entries.append(TlvEntry(type_id=tlv_type, data=tlv_data))
        off += tlv_len

    return area_end, entries


def parse_image(filename: str, data: bytes, manifest_entry: Optional[dict]) -> ParsedImage:
    offset = find_magic(data)
    if offset < 0:
        return ParsedImage(
            filename=filename,
            header=None,  # type: ignore[arg-type]
            protected_tlvs=[],
            unprotected_tlvs=[],
            manifest_entry=manifest_entry,
            error="MCUBoot magic (0x96f3b83d) not found in first 0x400 bytes",
        )

    if offset + IMAGE_HDR_SIZE > len(data):
        return ParsedImage(
            filename=filename,
            header=None,  # type: ignore[arg-type]
            protected_tlvs=[],
            unprotected_tlvs=[],
            manifest_entry=manifest_entry,
            error=f"File too short to contain a full image header at offset {offset:#x}",
        )

    header = parse_header(data, offset)

    tlv_start = offset + header.hdr_size + header.img_size
    protected_tlvs: list[TlvEntry] = []

    if header.protected_tlv_size > 0:
        tlv_start_after_prot, protected_tlvs = parse_tlv_area(data, tlv_start)
    else:
        tlv_start_after_prot = tlv_start

    _, unprotected_tlvs = parse_tlv_area(data, tlv_start_after_prot)

    return ParsedImage(
        filename=filename,
        header=header,
        protected_tlvs=protected_tlvs,
        unprotected_tlvs=unprotected_tlvs,
        manifest_entry=manifest_entry,
    )


# ── Formatting helpers ────────────────────────────────────────────────────────

W = 64  # output width

def sep(char="═") -> str:
    return char * W


def flags_str(flags: int) -> str:
    if flags == 0:
        return "0x0 (none)"
    names = [name for mask, name in IMAGE_FLAGS.items() if flags & mask]
    return f"{flags:#010x}  ({', '.join(names)})"


def format_tlv_data(entry: TlvEntry) -> list[str]:
    """Return formatted lines for a TLV's data payload."""
    t = entry.type_id
    data = entry.data

    if t in SHA_TYPES:
        return [data.hex()]

    if t == 0x01:  # KEYHASH
        return [data.hex()]

    if t == 0x40:  # DEPENDENCY
        if len(data) >= 12:
            img_id, slot, _, v_maj, v_min, v_rev, v_build = struct.unpack_from("<BB2xBBHI", data)
            ver = f"{v_maj}.{v_min}.{v_rev}+{v_build}"
            return [f"image_id={img_id}  slot={slot}  min_version={ver}"]
        return [data.hex()]

    if t in (0x50, 0x70, 0x73):  # SEC_CNT, DECOMP_SIZE, COMP_DEC_SIZE
        if len(data) >= 4:
            val, = struct.unpack_from("<I", data)
            return [f"{val}  ({val:#010x})"]
        return [data.hex()]

    # Generic hex dump, 16 bytes per line
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i: i + 16]
        lines.append(" ".join(f"{b:02x}" for b in chunk))
    return lines or ["(empty)"]


def manifest_version(entry: Optional[dict]) -> str:
    """Extract version string from a manifest entry (handles both key variants)."""
    if entry is None:
        return "(not in manifest)"
    return str(entry.get("version_MCUBOOT") or entry.get("version") or "(none)")


def version_match(hdr_ver: ImageVersion, manifest_ver: str) -> bool:
    return str(hdr_ver) == manifest_ver


# ── Printing ──────────────────────────────────────────────────────────────────

def print_image(img: ParsedImage) -> None:
    print()
    print(sep())
    print(f" {img.filename}")
    print(sep())

    if img.error:
        print(f"  ERROR: {img.error}")
        return

    h = img.header
    m_ver = manifest_version(img.manifest_entry)
    h_ver = str(h.version)
    match = version_match(h.version, m_ver)
    match_tag = "✓ MATCH" if match else "✗ MISMATCH"

    print(" Image Header")
    print(f"   magic:              {h.magic:#010x}  ✓")
    if h.offset > 0:
        print(f"   magic offset:       {h.offset:#010x}  (image preceded by {h.offset} padding bytes)")
    print(f"   version:            {h_ver}")
    print(f"   manifest version:   {m_ver:<16}  {match_tag}")
    print(f"   load_addr:          {h.load_addr:#010x}")
    print(f"   hdr_size:           {h.hdr_size:#06x}  ({h.hdr_size} bytes)")
    print(f"   img_size:           {h.img_size:#010x}  ({h.img_size} bytes)")
    print(f"   protected_tlv_size: {h.protected_tlv_size:#06x}")
    print(f"   flags:              {flags_str(h.flags)}")

    def print_tlv_section(label: str, entries: list[TlvEntry]) -> None:
        if not entries:
            return
        print()
        print(f" {label}")
        for entry in entries:
            type_label = f"{entry.name} ({entry.type_id:#04x})"
            print(f"   [{type_label}]  len={len(entry.data)}")
            for line in format_tlv_data(entry):
                print(f"     {line}")

    if img.protected_tlvs:
        print_tlv_section("Protected TLV Area", img.protected_tlvs)
    print_tlv_section("TLV Area", img.unprotected_tlvs)
    if not img.unprotected_tlvs and not img.protected_tlvs:
        print()
        print("  (no TLV entries found)")


def print_summary(images: list[ParsedImage]) -> None:
    print()
    print(sep("─"))
    print(" Summary")
    print(sep("─"))
    col_file = 34
    col_hdr = 16
    col_man = 16
    hdr = f"  {'File':<{col_file}}  {'Hdr Version':<{col_hdr}}  {'Manifest Ver':<{col_man}}  Match"
    print(hdr)
    print(f"  {'-'*col_file}  {'-'*col_hdr}  {'-'*col_man}  -----")
    for img in images:
        if img.error:
            print(f"  {img.filename:<{col_file}}  {'ERROR':<{col_hdr}}  {'':>{col_man}}  ✗")
            continue
        h_ver = str(img.header.version)
        m_ver = manifest_version(img.manifest_entry)
        ok = "✓" if version_match(img.header.version, m_ver) else "✗ MISMATCH"
        print(f"  {img.filename:<{col_file}}  {h_ver:<{col_hdr}}  {m_ver:<{col_man}}  {ok}")
    print(sep("─"))


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Dump MCUBoot TLV and version info from all binaries in a dfu_application.zip"
    )
    parser.add_argument(
        "zipfile",
        nargs="?",
        default="build/dfu_application.zip",
        help="Path to dfu_application.zip (default: build/dfu_application.zip)",
    )
    args = parser.parse_args()

    zip_path = Path(args.zipfile)
    if not zip_path.exists():
        print(f"ERROR: {zip_path} not found")
        raise SystemExit(1)

    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()

        # Load manifest
        manifest_by_file: dict[str, dict] = {}
        if "manifest.json" in names:
            manifest = json.loads(zf.read("manifest.json"))
            for entry in manifest.get("files", []):
                manifest_by_file[entry["file"]] = entry
            print(f"DFU bundle: {zip_path}")
            print(f"Bundle name: {manifest.get('name', '(unknown)')}")
            print(f"Files in zip: {', '.join(names)}")
        else:
            print(f"DFU bundle: {zip_path}  (no manifest.json)")

        bin_files = [n for n in names if n.endswith(".bin")]
        if not bin_files:
            print("No .bin files found in zip.")
            raise SystemExit(0)

        images: list[ParsedImage] = []
        for name in bin_files:
            data = zf.read(name)
            img = parse_image(name, data, manifest_by_file.get(name))
            images.append(img)
            print_image(img)

    print_summary(images)


if __name__ == "__main__":
    main()
