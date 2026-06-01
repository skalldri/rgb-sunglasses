"""Unit tests for the Bad Apple video conversion script."""

import pytest
import struct
import tempfile
from pathlib import Path
from PIL import Image
import sys

# Import the conversion script's functions
sys.path.insert(0, str(Path(__file__).parent.parent))
from convert_bad_apple import (
    pack_frame, floyd_steinberg, write_glim_header,
    GLIM_MAGIC, GLIM_VERSION, GLIM_HEADER_SIZE, GLIM_FRAME_FORMAT_RAW,
    TARGET_W, TARGET_H, FRAME_BYTES
)


class TestPackFrame:
    """Test frame bitpacking (MSB-first)."""

    def test_pack_frame_all_black(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        packed = pack_frame(img)
        assert len(packed) == FRAME_BYTES
        assert packed == b'\x00' * FRAME_BYTES

    def test_pack_frame_all_white(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 255)
        packed = pack_frame(img)
        assert len(packed) == FRAME_BYTES
        assert packed == b'\xff' * FRAME_BYTES

    def test_pack_frame_first_pixel(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        pixels[0, 0] = 255  # (0,0) is white
        packed = pack_frame(img)
        assert packed[0] == 0x80  # MSB of first byte
        assert packed[1:] == b'\x00' * (FRAME_BYTES - 1)

    def test_pack_frame_second_pixel(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        pixels[1, 0] = 255  # (1,0) is white
        packed = pack_frame(img)
        assert packed[0] == 0x40  # Second MSB of first byte
        assert packed[1:] == b'\x00' * (FRAME_BYTES - 1)

    def test_pack_frame_ninth_pixel(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        pixels[8, 0] = 255  # (8,0) is white
        packed = pack_frame(img)
        assert packed[0] == 0x00
        assert packed[1] == 0x80  # MSB of second byte
        assert packed[2:] == b'\x00' * (FRAME_BYTES - 2)

    def test_pack_frame_last_pixel(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        pixels[39, 11] = 255  # Last pixel
        packed = pack_frame(img)
        assert packed[-1] == 0x01  # LSB of last byte
        assert packed[:-1] == b'\x00' * (FRAME_BYTES - 1)

    def test_pack_frame_row_boundary(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        pixels[39, 0] = 255  # Last pixel of first row
        packed = pack_frame(img)
        assert packed[4] == 0x01  # bit 0 of byte 4 (pixels 32-39)
        assert (packed[5] & 0x80) == 0  # First bit of byte 5 (start of row 1) is 0

    def test_pack_frame_threshold_at_127(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        pixels[0, 0] = 127  # Below threshold
        pixels[1, 0] = 128  # At/above threshold
        packed = pack_frame(img)
        assert packed[0] == 0x40  # Only pixel 1 is on


class TestFloydSteinberg:
    """Test Floyd-Steinberg dithering."""

    def test_dither_all_white(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 255)
        dithered = floyd_steinberg(img)
        assert dithered.mode == '1'
        assert dithered.size == (TARGET_W, TARGET_H)
        # All white should remain all white (255 in 1-bit mode)
        assert all(p != 0 for p in dithered.getdata())

    def test_dither_all_black(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        dithered = floyd_steinberg(img)
        assert dithered.mode == '1'
        # All black should remain all black
        assert list(dithered.getdata()) == [0] * (TARGET_W * TARGET_H)

    def test_dither_mid_gray(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 128)
        dithered = floyd_steinberg(img)
        data = list(dithered.getdata())
        # Mid-gray should produce a dithered pattern (not all 0 or all 255)
        assert 0 in data  # Some black pixels
        assert 255 in data  # Some white pixels (255 in 1-bit mode)
        # Roughly balanced (mid-gray should be ~50% on)
        on_count = sum(1 for p in data if p != 0)
        total = TARGET_W * TARGET_H
        assert 0.3 * total < on_count < 0.7 * total


class TestGlimHeader:
    """Test GLIM header writing."""

    def test_header_magic(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 100, 24)
        with open(fname, 'rb') as f:
            magic = struct.unpack('<I', f.read(4))[0]
        assert magic == GLIM_MAGIC
        Path(fname).unlink()

    def test_header_version(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 100, 24)
        with open(fname, 'rb') as f:
            f.read(4)  # Skip magic
            version = struct.unpack('<B', f.read(1))[0]
        assert version == GLIM_VERSION
        Path(fname).unlink()

    def test_header_size(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 100, 24)
        with open(fname, 'rb') as f:
            f.read(5)  # Skip magic + version
            header_size = struct.unpack('<B', f.read(1))[0]
        assert header_size == GLIM_HEADER_SIZE
        Path(fname).unlink()

    def test_header_format(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 100, 24)
        with open(fname, 'rb') as f:
            f.read(6)  # Skip magic + version + header_size
            fmt = struct.unpack('<B', f.read(1))[0]
        assert fmt == GLIM_FRAME_FORMAT_RAW
        Path(fname).unlink()

    def test_header_fps(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 100, 30)
        with open(fname, 'rb') as f:
            f.read(7)  # Skip to fps field
            fps = struct.unpack('<B', f.read(1))[0]
        assert fps == 30
        Path(fname).unlink()

    def test_header_frame_count(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 5258, 24)
        with open(fname, 'rb') as f:
            f.read(12)  # Skip to frame_count field
            frame_count = struct.unpack('<I', f.read(4))[0]
        assert frame_count == 5258
        Path(fname).unlink()

    def test_header_frame_data_offset(self):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            fname = f.name
            write_glim_header(f, 100, 24)
        with open(fname, 'rb') as f:
            f.read(16)  # Skip to frame_data_offset field
            offset = struct.unpack('<I', f.read(4))[0]
        assert offset == GLIM_HEADER_SIZE
        Path(fname).unlink()


class TestRoundTrip:
    """Test pack->unpack round trips."""

    def test_known_pattern_single_pixel(self):
        img = Image.new('L', (TARGET_W, TARGET_H), 0)
        pixels = img.load()
        # Set a few specific pixels to white
        pixels[0, 0] = 255
        pixels[10, 5] = 255
        pixels[39, 11] = 255

        packed = pack_frame(img)

        # Verify by unpacking
        def get_pixel(buf, x, y):
            bitIdx = y * TARGET_W + x
            return (buf[bitIdx // 8] >> (7 - (bitIdx % 8))) & 1

        assert get_pixel(packed, 0, 0) == 1
        assert get_pixel(packed, 10, 5) == 1
        assert get_pixel(packed, 39, 11) == 1
        assert get_pixel(packed, 1, 0) == 0
        assert get_pixel(packed, 10, 4) == 0
