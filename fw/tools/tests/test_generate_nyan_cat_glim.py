"""Tests for tools/generate_nyan_cat_glim.py.

Tests cover make_frame() pixel art generation and write_glim() GLIM output.
Imported directly so the module's `if __name__ == '__main__': main()` guard
prevents the CLI from running during import.
"""

import io
import struct
import sys
import tempfile
from pathlib import Path

import pytest

# Allow importing from the tools directory
sys.path.insert(0, str(Path(__file__).parent.parent))

from generate_nyan_cat_glim import (
    BG,
    CAT_X,
    GLIM_FMT_RGB24,
    GLIM_HEADER_SIZE,
    GLIM_MAGIC,
    GLIM_VERSION,
    GRAY,
    RAINBOW,
    RAINBOW_Y0,
    RAINBOW_Y1,
    STARS,
    TARGET_H,
    TARGET_W,
    WHITE,
    DIM_STAR,
    make_frame,
    write_glim,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def pixel(img, x, y):
    """Return the (R, G, B) tuple of pixel (x, y) from a PIL image."""
    return img.getpixel((x, y))


# ---------------------------------------------------------------------------
# make_frame() — output geometry
# ---------------------------------------------------------------------------

def test_make_frame_returns_rgb_image():
    img = make_frame(0)
    assert img.mode == "RGB"


def test_make_frame_size():
    img = make_frame(0)
    assert img.size == (TARGET_W, TARGET_H)


def test_make_frame_produces_12_frames_without_error():
    frames = [make_frame(i) for i in range(12)]
    assert len(frames) == 12
    for f in frames:
        assert f.size == (TARGET_W, TARGET_H)


# ---------------------------------------------------------------------------
# make_frame() — background
# ---------------------------------------------------------------------------

def test_make_frame_background_pixel_is_dark_blue():
    """Pixel (0, 0) is not a star, not in a rainbow row, not part of the cat
    → it must equal BG."""
    img = make_frame(0)
    # Confirm (0,0) has no star in STARS and is in the dark zone
    star_positions = {(sx, sy) for sx, sy, _ in STARS}
    assert (0, 0) not in star_positions
    assert 0 < RAINBOW_Y0  # row 0 is dark
    assert pixel(img, 0, 0) == BG


def test_make_frame_dark_row_below_rainbow_is_bg():
    """Row 11 (below rainbow) at x=0 (no cat, no star listed there) is BG."""
    img = make_frame(0)
    star_positions = {(sx, sy) for sx, sy, _ in STARS}
    x, y = 0, TARGET_H - 1
    if (x, y) not in star_positions and x < CAT_X:
        assert pixel(img, x, y) == BG


# ---------------------------------------------------------------------------
# make_frame() — rainbow rows
# ---------------------------------------------------------------------------

def test_make_frame_rainbow_rows_not_background():
    """All pixels in rows RAINBOW_Y0..RAINBOW_Y1-1 at x=0 should be rainbow
    colors, not the BG color."""
    img = make_frame(0)
    for y in range(RAINBOW_Y0, RAINBOW_Y1):
        p = pixel(img, 0, y)
        assert p != BG, f"Row {y} col 0 should be rainbow, not BG"


def test_make_frame_rainbow_colors_are_from_palette():
    """Every pixel in the rainbow zone at x=0 should match a known rainbow entry."""
    img = make_frame(0)
    rainbow_set = set(RAINBOW)
    for y in range(RAINBOW_Y0, RAINBOW_Y1):
        p = pixel(img, 0, y)
        assert p in rainbow_set, f"Row {y} col 0: {p} not in RAINBOW palette"


def test_make_frame_rainbow_scrolls_between_frames():
    """The rainbow color at a fixed position changes as frame_idx increases."""
    colors = [pixel(make_frame(i), 0, RAINBOW_Y0) for i in range(6)]
    # Scroll is 1 stripe per 2 frames; after 2 frames the stripe at row 3 changes
    assert colors[0] != colors[2], "Rainbow color at (0, RAINBOW_Y0) should shift by frame 2"


def test_make_frame_rainbow_does_not_extend_past_rainbow_y1():
    """Row RAINBOW_Y1 (first row after rainbow) at x=0 must be background."""
    img = make_frame(0)
    star_positions = {(sx, sy) for sx, sy, _ in STARS}
    y = RAINBOW_Y1
    if (0, y) not in star_positions and 0 < CAT_X:
        p = pixel(img, 0, y)
        assert p == BG, f"Row {RAINBOW_Y1} should be BG, got {p}"


# ---------------------------------------------------------------------------
# make_frame() — cat sprite
# ---------------------------------------------------------------------------

def test_make_frame_cat_ear_pixels_are_gray():
    """BODY[0] has GRAY at sprite cols 7 and 8 — ears at the top of the head."""
    img = make_frame(0)
    ear_x = CAT_X + 7
    assert pixel(img, ear_x, 0) == GRAY, f"Ear at ({ear_x}, 0) should be GRAY"


def test_make_frame_cat_body_row_is_not_background():
    """Pop-tart row (sprite row 4) must not be BG at sprite col 1 (which has PINK)."""
    img = make_frame(0)
    # BODY[4] = [N, P, P, P, ...] — col 1 has PINK
    x = CAT_X + 1
    y = 4
    assert pixel(img, x, y) != BG, f"Pop-tart at ({x},{y}) should not be BG"


def test_make_frame_transparent_sprite_pixels_show_background():
    """Sprite col 14 row 0 is None (transparent) → should show BG (it's in dark zone)."""
    img = make_frame(0)
    # BODY[0] col 14 = N, row 0 is dark (not rainbow), no star at CAT_X+14 = 39, row 0
    x = CAT_X + 14
    y = 0
    star_positions = {(sx, sy) for sx, sy, _ in STARS}
    if (x, y) not in star_positions:
        assert pixel(img, x, y) == BG, f"Transparent sprite pixel ({x},{y}) should be BG"


# ---------------------------------------------------------------------------
# make_frame() — legs animation
# ---------------------------------------------------------------------------

def test_make_frame_legs_cycle_between_positions():
    """Leg pixel at sprite col 2, row 8 is GRAY in LEGS_A (frames 0-2) but not in LEGS_B (frames 3-5)."""
    # LEGS_A[0] col 2 = GRAY; LEGS_B[0] col 2 = None
    x = CAT_X + 2
    y = 8
    img_a = make_frame(0)  # leg_pos = (0//3)%2 = 0 → LEGS_A
    img_b = make_frame(3)  # leg_pos = (3//3)%2 = 1 → LEGS_B
    assert pixel(img_a, x, y) == GRAY, f"Frame 0 leg at ({x},{y}) should be GRAY (LEGS_A)"
    assert pixel(img_b, x, y) != GRAY, f"Frame 3 leg at ({x},{y}) should not be GRAY (LEGS_B)"


def test_make_frame_legs_repeat_after_six_frames():
    """Leg position cycles with period 6 (3 frames × 2 positions)."""
    x = CAT_X + 2
    y = 8
    p0 = pixel(make_frame(0), x, y)
    p6 = pixel(make_frame(6), x, y)
    assert p0 == p6, "Leg position should repeat every 6 frames"


# ---------------------------------------------------------------------------
# make_frame() — stars
# ---------------------------------------------------------------------------

def test_make_frame_star_is_white_on_even_phase():
    """Star (4, 0, phase=0): frame 0 → (0+0)%2==0 → WHITE."""
    img = make_frame(0)
    assert pixel(img, 4, 0) == WHITE, "Star (4,0) with phase 0 should be WHITE in frame 0"


def test_make_frame_star_is_dim_on_odd_phase():
    """Star (4, 0, phase=0): frame 1 → (1+0)%2==1 → DIM_STAR."""
    img = make_frame(1)
    assert pixel(img, 4, 0) == DIM_STAR, "Star (4,0) with phase 0 should be DIM_STAR in frame 1"


def test_make_frame_star_with_phase1_inverts_blink():
    """Star (12, 0, phase=1): frame 0 → (0+1)%2==1 → DIM_STAR; frame 1 → WHITE."""
    img0 = make_frame(0)
    img1 = make_frame(1)
    assert pixel(img0, 12, 0) == DIM_STAR, "Phase-1 star should be DIM in frame 0"
    assert pixel(img1, 12, 0) == WHITE,    "Phase-1 star should be WHITE in frame 1"


# ---------------------------------------------------------------------------
# write_glim() — file structure
# ---------------------------------------------------------------------------

def _write_and_read(frames, fps=12):
    """Write frames via write_glim() into a temp file and return the raw bytes."""
    with tempfile.NamedTemporaryFile(suffix=".glim", delete=False) as tf:
        path = Path(tf.name)
    write_glim(frames, fps, path)
    data = path.read_bytes()
    path.unlink()
    return data


def test_write_glim_header_magic():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    # Magic 0x474C494D stored as little-endian uint32 → bytes 'MILG'
    assert struct.unpack_from("<I", data, 0)[0] == GLIM_MAGIC


def test_write_glim_header_version():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    assert data[4] == GLIM_VERSION


def test_write_glim_header_size_field():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    assert data[5] == GLIM_HEADER_SIZE


def test_write_glim_header_format_is_rgb24():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    assert data[6] == GLIM_FMT_RGB24


def test_write_glim_header_fps():
    frames = [make_frame(0)]
    data = _write_and_read(frames, fps=24)
    assert data[7] == 24


def test_write_glim_header_width():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    w = struct.unpack_from("<H", data, 8)[0]
    assert w == TARGET_W


def test_write_glim_header_height():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    h = struct.unpack_from("<H", data, 10)[0]
    assert h == TARGET_H


def test_write_glim_header_frame_count_single():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    count = struct.unpack_from("<I", data, 12)[0]
    assert count == 1


def test_write_glim_header_frame_count_twelve():
    frames = [make_frame(i) for i in range(12)]
    data = _write_and_read(frames)
    count = struct.unpack_from("<I", data, 12)[0]
    assert count == 12


def test_write_glim_header_frame_data_offset():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    offset = struct.unpack_from("<I", data, 16)[0]
    assert offset == GLIM_HEADER_SIZE


def test_write_glim_total_file_size():
    n = 5
    frames = [make_frame(i) for i in range(n)]
    data = _write_and_read(frames)
    expected = GLIM_HEADER_SIZE + n * TARGET_W * TARGET_H * 3
    assert len(data) == expected


def test_write_glim_frame_data_starts_at_offset():
    frames = [make_frame(0)]
    data = _write_and_read(frames)
    frame_bytes = TARGET_W * TARGET_H * 3
    assert len(data) == GLIM_HEADER_SIZE + frame_bytes


# ---------------------------------------------------------------------------
# write_glim() — frame pixel data round-trip
# ---------------------------------------------------------------------------

def test_write_glim_frame0_first_pixel_matches_make_frame():
    """The first pixel written to the file must match make_frame(0) pixel (0,0)."""
    img = make_frame(0)
    expected_r, expected_g, expected_b = img.getpixel((0, 0))

    frames = [img]
    data = _write_and_read(frames)
    r = data[GLIM_HEADER_SIZE + 0]
    g = data[GLIM_HEADER_SIZE + 1]
    b = data[GLIM_HEADER_SIZE + 2]
    assert (r, g, b) == (expected_r, expected_g, expected_b)


def test_write_glim_frame0_pixel_39_11_matches_make_frame():
    """Last pixel (39, 11) in the file matches make_frame(0)."""
    img = make_frame(0)
    expected = img.getpixel((39, 11))

    frames = [img]
    data = _write_and_read(frames)
    # Pixel index = 11*40 + 39 = 479; byte offset in frame = 479*3 = 1437
    offset = GLIM_HEADER_SIZE + 479 * 3
    assert (data[offset], data[offset+1], data[offset+2]) == expected


def test_write_glim_frame1_first_pixel_matches_make_frame():
    """Frame 1 starts at offset GLIM_HEADER_SIZE + frame_bytes."""
    frame_bytes = TARGET_W * TARGET_H * 3
    img0 = make_frame(0)
    img1 = make_frame(1)
    expected = img1.getpixel((0, 0))

    data = _write_and_read([img0, img1])
    offset = GLIM_HEADER_SIZE + frame_bytes
    assert (data[offset], data[offset+1], data[offset+2]) == expected


def test_write_glim_all_frame_pixels_match_make_frame():
    """Round-trip: every pixel in every frame matches the source images."""
    n = 4
    imgs = [make_frame(i) for i in range(n)]
    data = _write_and_read(imgs)
    frame_bytes = TARGET_W * TARGET_H * 3

    for fi, img in enumerate(imgs):
        base = GLIM_HEADER_SIZE + fi * frame_bytes
        raw = img.convert("RGB").tobytes()
        assert data[base:base + frame_bytes] == raw, f"Frame {fi} pixel data mismatch"
