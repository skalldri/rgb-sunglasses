#!/usr/bin/env python3
"""
Convert Bad Apple!! video to GLIM (Glasses LED Image Media) format.

GLIM is a single-file animation container for 40×12 monochrome LED displays.
Bitpacked frames with optional LZ4 compression.

Usage:
  python convert_bad_apple.py --output bad_apple.glim
  python convert_bad_apple.py --input video.mp4 --output bad_apple.glim --fps 24
  python convert_bad_apple.py --input video.mp4 --no-dither
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError as e:
    sys.exit(f"Missing dependency: {e}. Install with: pip install Pillow numpy")

# Target display geometry
TARGET_W = 40
TARGET_H = 12
FRAME_BYTES = (TARGET_W * TARGET_H + 7) // 8  # 60 bytes per frame

# GLIM format constants
GLIM_MAGIC = 0x474C494D  # 'GLIM'
GLIM_VERSION = 1
GLIM_HEADER_SIZE = 24
GLIM_FRAME_FORMAT_RAW = 1

# Bad Apple!! YouTube URL
BAD_APPLE_URL = "https://www.youtube.com/watch?v=FtutLA63Cp8"


def pack_frame(img: Image.Image) -> bytes:
    """Pack a 40×12 grayscale image into 60 bytes (MSB-first bitpacking)."""
    if img.size != (TARGET_W, TARGET_H):
        raise ValueError(f"Image must be {TARGET_W}×{TARGET_H}, got {img.size}")

    pixels = list(img.getdata())
    if len(pixels) != TARGET_W * TARGET_H:
        raise ValueError(f"Image has {len(pixels)} pixels, expected {TARGET_W * TARGET_H}")

    bits = [1 if p > 127 else 0 for p in pixels]
    result = bytearray(FRAME_BYTES)

    for i, bit in enumerate(bits):
        if bit:
            result[i // 8] |= 1 << (7 - (i % 8))

    return bytes(result)


def floyd_steinberg(img: Image.Image) -> Image.Image:
    """Apply Floyd-Steinberg dithering to a grayscale image."""
    if img.mode != 'L':
        img = img.convert('L')

    pixels = np.array(img, dtype=np.float32) / 255.0
    result = np.zeros_like(pixels)

    for y in range(TARGET_H):
        for x in range(TARGET_W):
            old = pixels[y, x]
            new = 1.0 if old >= 0.5 else 0.0
            result[y, x] = new
            err = old - new

            # Distribute error to neighbors
            if x + 1 < TARGET_W:
                pixels[y, x + 1] += err * 7.0 / 16.0
            if y + 1 < TARGET_H:
                if x > 0:
                    pixels[y + 1, x - 1] += err * 3.0 / 16.0
                pixels[y + 1, x] += err * 5.0 / 16.0
                if x + 1 < TARGET_W:
                    pixels[y + 1, x + 1] += err * 1.0 / 16.0

    # Convert back to PIL Image
    dithered_uint8 = (result * 255).astype(np.uint8)
    return Image.fromarray(dithered_uint8).convert('1')


def write_glim_header(f, frame_count: int, fps: int) -> None:
    """Write a 24-byte GLIM v1 RAW format header to a file object."""
    header = (
        struct.pack('<I', GLIM_MAGIC) +
        struct.pack('<B', GLIM_VERSION) +
        struct.pack('<B', GLIM_HEADER_SIZE) +
        struct.pack('<B', GLIM_FRAME_FORMAT_RAW) +
        struct.pack('<B', fps) +
        struct.pack('<H', TARGET_W) +
        struct.pack('<H', TARGET_H) +
        struct.pack('<I', frame_count) +
        struct.pack('<I', GLIM_HEADER_SIZE) +  # frame_data_offset
        struct.pack('<I', 0)  # reserved
    )
    assert len(header) == GLIM_HEADER_SIZE, f"Header size mismatch: {len(header)} != {GLIM_HEADER_SIZE}"
    f.write(header)


def download_video(output_path: Path) -> None:
    """Download Bad Apple!! from YouTube using yt-dlp."""
    print(f"Downloading Bad Apple!! from YouTube...")
    cmd = [
        "yt-dlp",
        "--format", "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best",
        "--output", str(output_path),
        BAD_APPLE_URL,
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"yt-dlp failed: {e.stderr.decode('utf-8', errors='replace')}")
    except FileNotFoundError:
        raise RuntimeError("yt-dlp not found. Install with: pip install yt-dlp")


def extract_frames(video_path: Path, fps: int):
    """Generator yielding PIL Images from video at target FPS."""
    cmd = [
        "ffmpeg",
        "-i", str(video_path),
        "-vf", f"fps={fps},scale={TARGET_W}:{TARGET_H}:flags=lanczos,format=gray",
        "-f", "rawvideo",
        "-pix_fmt", "gray",
        "pipe:1",
        "-loglevel", "error",
    ]

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        raise RuntimeError("ffmpeg not found. Install ffmpeg from https://ffmpeg.org/download.html")

    frame_size = TARGET_W * TARGET_H  # 480 bytes per frame, 8-bit gray

    while True:
        data = proc.stdout.read(frame_size)
        if len(data) < frame_size:
            break
        yield Image.frombytes('L', (TARGET_W, TARGET_H), data)

    proc.wait()
    if proc.returncode != 0:
        stderr = proc.stderr.read().decode('utf-8', errors='replace').strip()
        raise RuntimeError(f"ffmpeg exited with code {proc.returncode}\n{stderr}")


def main():
    parser = argparse.ArgumentParser(
        description="Convert video to GLIM (Glasses LED Image Media) format for Bad Apple animation"
    )
    parser.add_argument("--input", "-i", help="Input video file (skip yt-dlp download)")
    parser.add_argument("--output", "-o", default="bad_apple.glim", help="Output .glim file")
    parser.add_argument("--fps", type=int, default=24, help="Target playback fps (default: 24)")
    parser.add_argument("--no-dither", action="store_true", help="Hard threshold instead of Floyd-Steinberg")

    args = parser.parse_args()

    video_path = None
    temp_dir = None

    # Get video source
    if args.input:
        video_path = Path(args.input)
        if not video_path.exists():
            sys.exit(f"Input file not found: {args.input}")
    else:
        # Download from YouTube into a fresh temp directory so yt-dlp finds no
        # pre-existing file at the target path (which would cause it to skip).
        temp_dir = Path(tempfile.mkdtemp())
        video_path = temp_dir / "bad_apple.mp4"
        try:
            download_video(video_path)
        except Exception as e:
            shutil.rmtree(temp_dir, ignore_errors=True)
            sys.exit(f"Download failed: {e}")

    # Extract and convert frames
    output_path = Path(args.output)
    frames = []

    try:
        dither_method = "Floyd-Steinberg" if not args.no_dither else "hard threshold"
        print(f"Extracting frames at {args.fps} fps, {TARGET_W}×{TARGET_H}, {dither_method}...")

        for i, gray_img in enumerate(extract_frames(video_path, args.fps)):
            if args.no_dither:
                img_1bit = gray_img.point(lambda p: 255 if p > 127 else 0, '1')
            else:
                img_1bit = floyd_steinberg(gray_img)

            frames.append(pack_frame(img_1bit))

            if (i + 1) % 100 == 0:
                print(f"  Processed {i + 1} frames...", end='\r', flush=True)

        frame_count = len(frames)
        print(f"\nTotal frames: {frame_count}")

        file_size = GLIM_HEADER_SIZE + frame_count * FRAME_BYTES
        print(f"Output size:  {file_size} bytes ({file_size / 1024:.1f} KB)")

        # Write GLIM file
        with open(output_path, 'wb') as f:
            write_glim_header(f, frame_count, args.fps)
            for frame_data in frames:
                f.write(frame_data)

        print(f"Written to {output_path}")

    finally:
        # Clean up temp directory if we downloaded
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        sys.exit(f"Error: {e}")
