#!/usr/bin/env python3
"""
Convert an animated GIF to GLIM (Glasses LED Image Media) format.

Supports both monochrome (Raw, 1-bit) and full-colour (Rgb24, 3-byte) output.

Display geometry: 40 × 12 pixels. Nose cutout: 10 × 6 px at center-bottom
  (x=15–24, y=6–11). Use --x-offset to shift content away from the cutout.

For Nyan Cat specifically, prefer the dedicated procedural generator:
  python tools/generate_nyan_cat_glim.py
That script draws the sprite pixel-by-pixel instead of scaling a GIF, which
produces a much better result on the 40×12 display.

Usage examples:
  # Download and convert a GIF (RGB24, right-biased for nose cutout)
  python convert_gif_to_glim.py --output animation.glim

  # Convert a local GIF to monochrome with a custom on-colour
  python convert_gif_to_glim.py --input anim.gif --mono --mono-color 255,0,128

  # Convert with a 10-pixel right-offset so content clears the nose cutout
  python convert_gif_to_glim.py --input anim.gif --x-offset 10
"""

import argparse
import struct
import sys
import urllib.request
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Missing dependency: Pillow. Install with: pip install Pillow")

# ---------------------------------------------------------------------------
# Display geometry (matches src/led_config.h)
# ---------------------------------------------------------------------------
TARGET_W = 40
TARGET_H = 12
NOSE_CUTOUT_X = 15   # kFrameNoseCutoutX
NOSE_CUTOUT_Y = 6    # kFrameNoseCutoutY
NOSE_CUTOUT_W = 10   # kFrameNoseCutoutWidth
NOSE_CUTOUT_H = 6    # kFrameNoseCutoutHeight

# ---------------------------------------------------------------------------
# GLIM format constants (mirrors src/storage/glim_decoder.h)
# ---------------------------------------------------------------------------
GLIM_MAGIC             = 0x474C494D   # 'GLIM' little-endian
GLIM_VERSION           = 1
GLIM_HEADER_SIZE       = 24
GLIM_FRAME_FORMAT_RAW  = 1
GLIM_FRAME_FORMAT_RGB24 = 3

# Default NyanCat GIF URL
NYAN_CAT_URL = "http://www.nyan.cat/cats/original.gif"


# ---------------------------------------------------------------------------
# Pure functions (importable for pytest)
# ---------------------------------------------------------------------------

def pack_frame_mono(img: Image.Image) -> bytes:
    """Pack a 40×12 grayscale (or 1-bit) image to Raw bitpacked bytes (MSB-first)."""
    if img.size != (TARGET_W, TARGET_H):
        raise ValueError(f"Expected {TARGET_W}×{TARGET_H}, got {img.size}")
    gray = img.convert('L')
    pixels = list(gray.getdata())
    frame_bytes = (TARGET_W * TARGET_H + 7) // 8
    result = bytearray(frame_bytes)
    for i, p in enumerate(pixels):
        if p > 127:
            result[i // 8] |= 1 << (7 - (i % 8))
    return bytes(result)


def pack_frame_rgb24(img: Image.Image) -> bytes:
    """Pack a 40×12 image to RGB24 bytes (R,G,B per pixel, row-major)."""
    if img.size != (TARGET_W, TARGET_H):
        raise ValueError(f"Expected {TARGET_W}×{TARGET_H}, got {img.size}")
    rgb = img.convert('RGB')
    return bytes(rgb.tobytes())


def write_glim_header(f, frame_count: int, fps: int, frame_format: int,
                      mono_color: tuple[int, int, int] = (0, 0, 0)) -> None:
    """Write a 24-byte GLIM v1 header.

    mono_color: (R, G, B) for the "on" pixel colour in mono formats.
    (0,0,0) is the sentinel meaning "default white" and is the correct value
    to write for Rgb24 frames (where per-pixel colour is stored in the frame).
    """
    header = (
        struct.pack('<I', GLIM_MAGIC) +
        struct.pack('<B', GLIM_VERSION) +
        struct.pack('<B', GLIM_HEADER_SIZE) +
        struct.pack('<B', frame_format) +
        struct.pack('<B', fps) +
        struct.pack('<H', TARGET_W) +
        struct.pack('<H', TARGET_H) +
        struct.pack('<I', frame_count) +
        struct.pack('<I', GLIM_HEADER_SIZE) +   # frame_data_offset
        struct.pack('<BBB', *mono_color) +       # bytes 20-22: mono colour
        struct.pack('<B', 0)                     # byte 23: reserved
    )
    assert len(header) == GLIM_HEADER_SIZE
    f.write(header)


def resize_with_offset(img: Image.Image, x_offset: int) -> Image.Image:
    """Scale img to (TARGET_W + x_offset) × TARGET_H, then crop the rightmost TARGET_W columns.

    A positive x_offset shifts the content to the right on the display, useful for
    keeping the cat body away from the center nose cutout.
    """
    scale_w = TARGET_W + x_offset
    scaled = img.resize((scale_w, TARGET_H), Image.LANCZOS)
    return scaled.crop((x_offset, 0, x_offset + TARGET_W, TARGET_H))


def gif_frames(path: Path) -> list[Image.Image]:
    """Return all frames of an animated GIF as RGBA PIL Images."""
    frames = []
    with Image.open(path) as gif:
        try:
            while True:
                frame = gif.copy().convert('RGBA')
                frames.append(frame)
                gif.seek(gif.tell() + 1)
        except EOFError:
            pass
    return frames


def gif_fps(path: Path) -> int:
    """Return the GIF's native frame rate (clamped to 1–60 fps)."""
    with Image.open(path) as gif:
        try:
            duration_ms = gif.info.get('duration', 100)
        except Exception:
            duration_ms = 100
    fps = max(1, min(60, round(1000 / max(1, duration_ms))))
    return fps


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def download_gif(url: str, dest: Path) -> None:
    print(f"Downloading {url} ...")
    urllib.request.urlretrieve(url, dest)


def main():
    parser = argparse.ArgumentParser(
        description="Convert an animated GIF to GLIM format for the LED sunglasses display."
    )
    parser.add_argument('--input', '-i', help='Local GIF file (skip download)')
    parser.add_argument('--output', '-o', default='nyan_cat.glim', help='Output .glim file')
    parser.add_argument('--url', default=NYAN_CAT_URL,
                        help=f'GIF URL to download (default: Nyan Cat original)')
    parser.add_argument('--fps', type=int, default=0,
                        help='Override playback FPS (0 = use GIF native rate)')
    parser.add_argument('--x-offset', type=int, default=10,
                        help='Shift content N pixels right to clear the nose cutout (default: 10)')
    parser.add_argument('--mono', action='store_true',
                        help='Output 1-bit monochrome (Raw format) instead of RGB24')
    parser.add_argument('--mono-color', default='255,255,255',
                        help='RGB colour for lit pixels in mono mode, e.g. 255,0,128 (default: white)')
    args = parser.parse_args()

    # Resolve input GIF
    if args.input:
        gif_path = Path(args.input)
        if not gif_path.exists():
            sys.exit(f"Input file not found: {args.input}")
    else:
        gif_path = Path('/tmp/_glim_download.gif')
        try:
            download_gif(args.url, gif_path)
        except Exception as e:
            sys.exit(f"Download failed: {e}")

    # Parse mono color
    try:
        mono_color = tuple(int(v) for v in args.mono_color.split(','))
        if len(mono_color) != 3 or not all(0 <= c <= 255 for c in mono_color):
            raise ValueError
    except (ValueError, TypeError):
        sys.exit("--mono-color must be three comma-separated integers 0-255, e.g. 255,0,128")

    # Determine FPS
    fps = args.fps if args.fps > 0 else gif_fps(gif_path)
    print(f"FPS: {fps}")

    # Load frames
    print(f"Loading frames from {gif_path} ...")
    raw_frames = gif_frames(gif_path)
    print(f"  {len(raw_frames)} frames found")

    # Process frames
    fmt_name = "mono" if args.mono else "RGB24"
    print(f"Converting to {TARGET_W}×{TARGET_H} {fmt_name} "
          f"(x_offset={args.x_offset}) ...")

    packed_frames = []
    for frame in raw_frames:
        # Composite onto black background (handles transparency)
        bg = Image.new('RGBA', frame.size, (0, 0, 0, 255))
        bg.paste(frame, mask=frame.split()[3])
        composited = bg.convert('RGB')

        resized = resize_with_offset(composited, args.x_offset)

        if args.mono:
            packed_frames.append(pack_frame_mono(resized))
        else:
            packed_frames.append(pack_frame_rgb24(resized))

    frame_count = len(packed_frames)
    frame_bytes = len(packed_frames[0]) if packed_frames else 0
    file_size = GLIM_HEADER_SIZE + frame_count * frame_bytes

    print(f"Total frames: {frame_count}")
    print(f"Bytes/frame:  {frame_bytes}")
    print(f"Output size:  {file_size} bytes ({file_size / 1024:.1f} KB)")

    output_path = Path(args.output)
    with open(output_path, 'wb') as f:
        frame_format = GLIM_FRAME_FORMAT_RAW if args.mono else GLIM_FRAME_FORMAT_RGB24
        # mono_color sentinel: (0,0,0) means "default white" in the decoder.
        # Only write a non-zero color for mono mode with a non-white color.
        header_mono_color = (0, 0, 0)
        if args.mono and mono_color != (255, 255, 255):
            header_mono_color = mono_color
        write_glim_header(f, frame_count, fps, frame_format, header_mono_color)
        for frame_data in packed_frames:
            f.write(frame_data)

    print(f"Written to {output_path}")


if __name__ == '__main__':
    main()
