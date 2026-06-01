"""Unit tests for convert_gif_to_glim.py."""

import struct
import tempfile
from pathlib import Path

import pytest
from PIL import Image

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))
from convert_gif_to_glim import (
    pack_frame_mono, pack_frame_rgb24, write_glim_header, resize_with_offset,
    GLIM_MAGIC, GLIM_VERSION, GLIM_HEADER_SIZE,
    GLIM_FRAME_FORMAT_RAW, GLIM_FRAME_FORMAT_RGB24,
    TARGET_W, TARGET_H,
    NOSE_CUTOUT_X, NOSE_CUTOUT_Y, NOSE_CUTOUT_W, NOSE_CUTOUT_H,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def solid(color, mode='RGB'):
    return Image.new(mode, (TARGET_W, TARGET_H), color)


# ---------------------------------------------------------------------------
# pack_frame_mono
# ---------------------------------------------------------------------------

def mono(value: int) -> Image.Image:
    """Create a TARGET_W × TARGET_H grayscale image filled with `value`."""
    return Image.new('L', (TARGET_W, TARGET_H), value)


class TestPackFrameMono:
    def test_all_black(self):
        packed = pack_frame_mono(mono(0))
        assert packed == b'\x00' * ((TARGET_W * TARGET_H + 7) // 8)

    def test_all_white(self):
        packed = pack_frame_mono(mono(255))
        assert packed == b'\xff' * ((TARGET_W * TARGET_H + 7) // 8)

    def test_first_pixel(self):
        img = mono(0)
        img.putpixel((0, 0), 255)
        packed = pack_frame_mono(img)
        assert packed[0] == 0x80
        assert packed[1:] == b'\x00' * (len(packed) - 1)

    def test_last_pixel(self):
        img = mono(0)
        img.putpixel((TARGET_W - 1, TARGET_H - 1), 255)
        packed = pack_frame_mono(img)
        assert packed[-1] == 0x01
        assert packed[:-1] == b'\x00' * (len(packed) - 1)

    def test_threshold_at_127(self):
        img = mono(0)
        img.putpixel((0, 0), 127)  # below threshold → off
        img.putpixel((1, 0), 128)  # at threshold → on
        packed = pack_frame_mono(img)
        assert packed[0] == 0x40  # bit 6 (second pixel) set, bit 7 (first) clear

    def test_wrong_size_raises(self):
        with pytest.raises(ValueError):
            pack_frame_mono(Image.new('RGB', (10, 10), 0))


# ---------------------------------------------------------------------------
# pack_frame_rgb24
# ---------------------------------------------------------------------------

class TestPackFrameRgb24:
    def test_all_black(self):
        packed = pack_frame_rgb24(solid((0, 0, 0)))
        assert packed == b'\x00' * (TARGET_W * TARGET_H * 3)

    def test_all_white(self):
        packed = pack_frame_rgb24(solid((255, 255, 255)))
        assert packed == b'\xff' * (TARGET_W * TARGET_H * 3)

    def test_length(self):
        assert len(pack_frame_rgb24(solid((0, 0, 0)))) == TARGET_W * TARGET_H * 3

    def test_first_pixel_rgb(self):
        img = solid((0, 0, 0))
        img.putpixel((0, 0), (10, 20, 30))
        packed = pack_frame_rgb24(img)
        assert packed[0] == 10
        assert packed[1] == 20
        assert packed[2] == 30

    def test_second_pixel_rgb(self):
        img = solid((0, 0, 0))
        img.putpixel((1, 0), (100, 150, 200))
        packed = pack_frame_rgb24(img)
        assert packed[3] == 100
        assert packed[4] == 150
        assert packed[5] == 200

    def test_last_pixel_rgb(self):
        img = solid((0, 0, 0))
        img.putpixel((TARGET_W - 1, TARGET_H - 1), (11, 22, 33))
        packed = pack_frame_rgb24(img)
        assert packed[-3] == 11
        assert packed[-2] == 22
        assert packed[-1] == 33

    def test_second_row_offset(self):
        """First pixel of row 1 is at byte offset TARGET_W*3."""
        img = solid((0, 0, 0))
        img.putpixel((0, 1), (55, 66, 77))
        packed = pack_frame_rgb24(img)
        offset = TARGET_W * 3
        assert packed[offset]     == 55
        assert packed[offset + 1] == 66
        assert packed[offset + 2] == 77

    def test_wrong_size_raises(self):
        with pytest.raises(ValueError):
            pack_frame_rgb24(Image.new('RGB', (10, 10), 0))


# ---------------------------------------------------------------------------
# write_glim_header — mono color field
# ---------------------------------------------------------------------------

class TestGlimHeaderMonoColor:
    def _read_header(self, frame_count, fps, frame_format, mono_color=(0, 0, 0)):
        with tempfile.NamedTemporaryFile(mode='wb', delete=False, suffix='.glim') as f:
            fname = f.name
            write_glim_header(f, frame_count, fps, frame_format, mono_color)
        data = Path(fname).read_bytes()
        Path(fname).unlink()
        return data

    def test_magic(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW)
        assert struct.unpack('<I', data[0:4])[0] == GLIM_MAGIC

    def test_version(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW)
        assert data[4] == GLIM_VERSION

    def test_header_size(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW)
        assert data[5] == GLIM_HEADER_SIZE

    def test_frame_format_raw(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW)
        assert data[6] == GLIM_FRAME_FORMAT_RAW

    def test_frame_format_rgb24(self):
        data = self._read_header(1, 12, GLIM_FRAME_FORMAT_RGB24)
        assert data[6] == GLIM_FRAME_FORMAT_RGB24

    def test_fps(self):
        data = self._read_header(1, 30, GLIM_FRAME_FORMAT_RAW)
        assert data[7] == 30

    def test_frame_count(self):
        data = self._read_header(5258, 24, GLIM_FRAME_FORMAT_RAW)
        assert struct.unpack('<I', data[12:16])[0] == 5258

    def test_frame_data_offset_equals_header_size(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW)
        assert struct.unpack('<I', data[16:20])[0] == GLIM_HEADER_SIZE

    def test_mono_color_default_is_zero_sentinel(self):
        """Default (0,0,0) sentinel means 'white' to the decoder."""
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW)
        assert data[20] == 0
        assert data[21] == 0
        assert data[22] == 0

    def test_mono_color_custom(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW, mono_color=(255, 0, 128))
        assert data[20] == 255
        assert data[21] == 0
        assert data[22] == 128

    def test_mono_color_rgb24_is_zero(self):
        """RGB24 frames carry their own colour; header mono_color should be (0,0,0)."""
        data = self._read_header(1, 12, GLIM_FRAME_FORMAT_RGB24, mono_color=(0, 0, 0))
        assert data[20] == 0
        assert data[21] == 0
        assert data[22] == 0

    def test_reserved_byte_23_is_zero(self):
        data = self._read_header(1, 24, GLIM_FRAME_FORMAT_RAW, mono_color=(255, 128, 64))
        assert data[23] == 0


# ---------------------------------------------------------------------------
# resize_with_offset — nose cutout bias
# ---------------------------------------------------------------------------

class TestResizeWithOffset:
    def test_output_size_is_target(self):
        img = solid((128, 0, 128))
        out = resize_with_offset(img, 10)
        assert out.size == (TARGET_W, TARGET_H)

    def test_zero_offset_same_as_resize(self):
        img = solid((200, 100, 50))
        out = resize_with_offset(img, 0)
        assert out.size == (TARGET_W, TARGET_H)

    def test_nose_cutout_constants_match_led_config(self):
        """Confirm the Python constants mirror src/led_config.h."""
        assert NOSE_CUTOUT_X == (TARGET_W - NOSE_CUTOUT_W) // 2   # 15
        assert NOSE_CUTOUT_Y == TARGET_H - NOSE_CUTOUT_H          # 6
        assert NOSE_CUTOUT_W == 10
        assert NOSE_CUTOUT_H == 6
