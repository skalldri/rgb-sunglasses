#!/usr/bin/env python3
"""generate_nyan_cat_glim.py — Procedural Nyan Cat animation for the RGB sunglasses.

Generates nyan_cat.glim entirely from code — no internet access or GIF source
required.  The output is a 12-frame RGB24 GLIM file that reproduces the key
visual elements of the original animation:

  • Dark blue starfield background above and below the rainbow.
  • 6-colour horizontal rainbow trail filling the middle 6 rows of the display.
  • A hand-crafted 15 × 12 pixel-art Nyan Cat sprite (cat face on the right).
  • Twinkling stars in the dark zones.
  • Animated legs cycling through two positions.
  • The rainbow stripes scroll vertically, giving the "rushing through space" feel.

Display: 40 × 12.  Cat placed at columns 25–39 to clear the nose cutout
(x = 15–24, y = 6–11).

Usage:
  python tools/generate_nyan_cat_glim.py               # → nyan_cat.glim
  python tools/generate_nyan_cat_glim.py -o out.glim
  python tools/generate_nyan_cat_glim.py --fps 12 --frames 12
"""

import argparse
import struct
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    raise SystemExit("Missing dependency: pip install Pillow")

# ---------------------------------------------------------------------------
# Display constants — match src/led_config.h
# ---------------------------------------------------------------------------
TARGET_W = 40
TARGET_H = 12
CAT_X    = 25    # left column of the 15-wide cat sprite (display cols 25–39)

# ---------------------------------------------------------------------------
# GLIM format constants — match src/storage/glim_decoder.h
# ---------------------------------------------------------------------------
GLIM_MAGIC       = 0x474C494D
GLIM_VERSION     = 1
GLIM_HEADER_SIZE = 24
GLIM_FMT_RGB24   = 3

# ---------------------------------------------------------------------------
# Colours
# ---------------------------------------------------------------------------
BG       = (  0,  19,  51)   # dark blue background
GRAY     = (119, 119, 119)   # cat body / head / legs
WHITE    = (255, 255, 255)   # eye whites / bright stars
BLACK    = ( 17,  17,  17)   # pupils
PINK     = (255, 170, 204)   # pop-tart frosting
HOT      = (255,  51, 153)   # nose / icing accent
RED_S    = (255,   0,   0)   # red sprinkle
TEAL_S   = (  0, 204, 153)   # teal sprinkle
YELLOW_S = (255, 204,   0)   # yellow sprinkle
DIM_STAR = ( 70,  70, 100)   # dim/off-phase star

# 6 rainbow stripe colours, top → bottom (1 display row each = 6 rows total)
RAINBOW = [
    (255,   0,   0),   # red
    (255, 153,   0),   # orange
    (255, 255,   0),   # yellow
    ( 51, 204,   0),   # green
    ( 51, 153, 255),   # blue
    (153,  51, 255),   # violet
]

# Rainbow occupies the middle rows only — dark sky above and below
RAINBOW_Y0 = 3   # first rainbow row  (rows 0–2 are dark starfield)
RAINBOW_Y1 = 9   # first row after rainbow (rows 9–11 are dark starfield)

# Stars: (display_x, display_y, blink_phase)
# Rows 0–2: cutout doesn't apply (cutout is y=6–11), free to place anywhere x < CAT_X
# Rows 9–11: nose cutout covers x=15–24, so keep stars at x ≤ 14 for these rows
STARS = [
    ( 4,  0, 0), (12,  0, 1), (20,  0, 0), (23,  0, 1),
    ( 8,  1, 1), (17,  1, 0),
    ( 2,  2, 0), (14,  2, 1), (21,  2, 0),
    ( 6,  9, 0), (12,  9, 1),
    ( 3, 10, 1), (10, 10, 0),
    ( 7, 11, 0), (13, 11, 1),
]

# ---------------------------------------------------------------------------
# Cat sprite — 15 columns × 12 rows, placed at x = CAT_X (cols 25–39)
#
# The cat face is on the RIGHT portion of the sprite (cols 6–13) so it faces
# right, as in the original.  The pop-tart body spans the full width.
# None = transparent (background or rainbow visible through the sprite).
# ---------------------------------------------------------------------------
N = None
G = GRAY;  W = WHITE;  K = BLACK;  P = PINK;  H = HOT
R = RED_S; T = TEAL_S; Y = YELLOW_S

# Rows 0–7: head + pop-tart (same across all animation frames)
BODY = [
    #  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
    [ N, N, N, N, N, N, N, G, G, N, N, G, G, N, N],  # R0  ears
    [ N, N, N, N, N, N, G, G, G, G, G, G, G, G, N],  # R1  head
    [ N, N, N, N, N, G, G, W, K, G, G, W, K, G, N],  # R2  eyes
    [ N, N, N, N, N, G, G, G, G, H, G, G, G, G, N],  # R3  nose
    [ N, P, P, P, P, P, P, P, P, P, P, P, P, P, N],  # R4  pop-tart top
    [ P, P, R, P, T, P, Y, P, R, P, T, P, Y, P, N],  # R5  sprinkles
    [ P, Y, P, P, P, P, P, P, P, P, P, P, P, R, N],  # R6  sprinkles
    [ N, P, P, P, P, P, P, P, P, P, P, P, P, P, N],  # R7  pop-tart bottom
]

# Rows 8–11: legs — two alternating positions to animate a running cycle.
# Four legs at sprite columns 2, 5, 9, 12.
LEGS_A = [
    #  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
    [ N, N, G, N, N, G, N, N, N, G, N, N, G, N, N],  # R8
    [ N, N, G, N, N, G, N, N, N, G, N, N, G, N, N],  # R9
    [ N, N, G, N, N, G, N, N, N, G, N, N, G, N, N],  # R10
    [ N, N, N, G, N, G, N, N, N, N, G, N, G, N, N],  # R11  feet slightly splayed
]

LEGS_B = [
    #  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
    [ N, N, N, G, N, G, N, N, N, N, G, N, G, N, N],  # R8
    [ N, N, N, G, N, G, N, N, N, N, G, N, G, N, N],  # R9
    [ N, N, N, G, N, G, N, N, N, N, G, N, G, N, N],  # R10
    [ N, N, G, N, N, G, N, N, N, G, N, N, G, N, N],  # R11  feet back in
]

LEGS = [LEGS_A, LEGS_B]

NUM_FRAMES  = 12
DEFAULT_FPS = 12


# ---------------------------------------------------------------------------
# Frame generation
# ---------------------------------------------------------------------------

def make_frame(frame_idx: int) -> Image.Image:
    """Render a single animation frame."""
    img = Image.new('RGB', (TARGET_W, TARGET_H), BG)
    px  = img.load()

    # Twinkling stars in the dark zones above and below the rainbow
    for sx, sy, phase in STARS:
        px[sx, sy] = WHITE if (frame_idx + phase) % 2 == 0 else DIM_STAR

    # Rainbow stripes — middle 6 rows only (RAINBOW_Y0..RAINBOW_Y1-1)
    # Extend 1 column under the cat's transparent tail edge to avoid a hard cut.
    rainbow_end = CAT_X + 1
    for y in range(RAINBOW_Y0, RAINBOW_Y1):
        row_in_rainbow = y - RAINBOW_Y0
        # Vertical scroll: one stripe per 2 frames (smoother than 1/frame)
        effective_row = (row_in_rainbow + frame_idx // 2) % len(RAINBOW)
        color = RAINBOW[effective_row]
        for x in range(rainbow_end):
            px[x, y] = color

    # Cat sprite: body rows 0–7, then animated legs rows 8–11
    leg_pos = (frame_idx // 3) % 2
    sprite  = BODY + LEGS[leg_pos]

    for row_i, row_data in enumerate(sprite):
        for col_i, color in enumerate(row_data):
            if color is None:
                continue
            sx, sy = CAT_X + col_i, row_i
            if 0 <= sx < TARGET_W and 0 <= sy < TARGET_H:
                px[sx, sy] = color

    return img


# ---------------------------------------------------------------------------
# GLIM output
# ---------------------------------------------------------------------------

def write_glim(frames: list, fps: int, path: Path) -> None:
    """Write frames to an RGB24 GLIM file."""
    header = (
        struct.pack('<I', GLIM_MAGIC) +
        struct.pack('<B', GLIM_VERSION) +
        struct.pack('<B', GLIM_HEADER_SIZE) +
        struct.pack('<B', GLIM_FMT_RGB24) +
        struct.pack('<B', fps) +
        struct.pack('<H', TARGET_W) +
        struct.pack('<H', TARGET_H) +
        struct.pack('<I', len(frames)) +
        struct.pack('<I', GLIM_HEADER_SIZE) +   # frame_data_offset
        struct.pack('<BBB', 0, 0, 0) +           # mono_color (ignored for RGB24)
        struct.pack('<B',   0)                   # reserved
    )
    assert len(header) == GLIM_HEADER_SIZE, "header size mismatch"

    frame_size  = TARGET_W * TARGET_H * 3
    total_bytes = GLIM_HEADER_SIZE + len(frames) * frame_size

    with open(path, 'wb') as f:
        f.write(header)
        for img in frames:
            f.write(bytes(img.convert('RGB').tobytes()))

    print(f"Wrote {len(frames)} frames @ {fps} fps → {path} "
          f"({total_bytes} bytes, {total_bytes / 1024:.1f} KB)")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--output', '-o', default='nyan_cat.glim',
                        help='Output .glim file path (default: nyan_cat.glim)')
    parser.add_argument('--fps', type=int, default=DEFAULT_FPS,
                        help=f'Playback frame rate in fps (default: {DEFAULT_FPS})')
    parser.add_argument('--frames', type=int, default=NUM_FRAMES,
                        help=f'Number of frames to generate (default: {NUM_FRAMES})')
    args = parser.parse_args()

    print(f"Generating {args.frames}-frame Nyan Cat animation ({TARGET_W}×{TARGET_H}, RGB24) …")
    frames = [make_frame(i) for i in range(args.frames)]
    write_glim(frames, args.fps, Path(args.output))


if __name__ == '__main__':
    main()
