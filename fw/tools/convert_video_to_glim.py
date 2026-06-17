#!/usr/bin/env python3
"""
Convert a video (local file or URL) to RGB24 GLIM format for the LED sunglasses display.

Unlike convert_bad_apple.py (monochrome only), this writes full-colour Rgb24
frames — see fw/src/storage/GLIM_FORMAT.md section 3.3.

Usage:
  python convert_video_to_glim.py --url "https://..." --output animation.glim
  python convert_video_to_glim.py --input video.mp4 --output animation.glim --fps 24
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Target display geometry (matches src/led_config.h)
TARGET_W = 40
TARGET_H = 12
FRAME_BYTES = TARGET_W * TARGET_H * 3  # 1440 bytes/frame, RGB24

# GLIM format constants (mirrors src/storage/glim_decoder.h)
GLIM_MAGIC = 0x474C494D  # 'GLIM'
GLIM_VERSION = 1
GLIM_HEADER_SIZE = 24
GLIM_FRAME_FORMAT_RGB24 = 3


def write_glim_header(f, frame_count: int, fps: int) -> None:
    """Write a 24-byte GLIM v1 Rgb24 header to a file object."""
    header = (
        struct.pack('<I', GLIM_MAGIC) +
        struct.pack('<B', GLIM_VERSION) +
        struct.pack('<B', GLIM_HEADER_SIZE) +
        struct.pack('<B', GLIM_FRAME_FORMAT_RGB24) +
        struct.pack('<B', fps) +
        struct.pack('<H', TARGET_W) +
        struct.pack('<H', TARGET_H) +
        struct.pack('<I', frame_count) +
        struct.pack('<I', GLIM_HEADER_SIZE) +  # frame_data_offset
        struct.pack('<BBB', 0, 0, 0) +          # mono colour fields, ignored for Rgb24
        struct.pack('<B', 0)                    # reserved
    )
    assert len(header) == GLIM_HEADER_SIZE, f"Header size mismatch: {len(header)} != {GLIM_HEADER_SIZE}"
    f.write(header)


def download_video(url: str, output_path: Path) -> None:
    """Download a video via yt-dlp."""
    print(f"Downloading {url} ...")
    cmd = [
        "yt-dlp",
        "--format", "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best",
        "--output", str(output_path),
        url,
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"yt-dlp failed: {e.stderr.decode('utf-8', errors='replace')}")
    except FileNotFoundError:
        raise RuntimeError("yt-dlp not found. Install with: pip install yt-dlp")


def extract_frames_rgb24(video_path: Path, fps: int):
    """Generator yielding raw RGB24 frame bytes (1440 bytes each) at TARGET_W x TARGET_H."""
    cmd = [
        "ffmpeg",
        "-i", str(video_path),
        "-vf", f"fps={fps},scale={TARGET_W}:{TARGET_H}:flags=lanczos,format=rgb24",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "pipe:1",
        "-loglevel", "error",
    ]

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        raise RuntimeError("ffmpeg not found. Install ffmpeg from https://ffmpeg.org/download.html")

    while True:
        data = proc.stdout.read(FRAME_BYTES)
        if len(data) < FRAME_BYTES:
            break
        yield data

    proc.wait()
    if proc.returncode != 0:
        stderr = proc.stderr.read().decode('utf-8', errors='replace').strip()
        raise RuntimeError(f"ffmpeg exited with code {proc.returncode}\n{stderr}")


def main():
    parser = argparse.ArgumentParser(
        description="Convert a video to RGB24 GLIM format for the LED sunglasses display"
    )
    parser.add_argument("--input", "-i", help="Local video file (skip yt-dlp download)")
    parser.add_argument("--url", help="Video URL to download via yt-dlp")
    parser.add_argument("--output", "-o", default="output.glim", help="Output .glim file")
    parser.add_argument("--fps", type=int, default=24, help="Target playback fps (default: 24)")

    args = parser.parse_args()

    if not args.input and not args.url:
        sys.exit("Must specify --input or --url")

    video_path = None
    temp_dir = None

    if args.input:
        video_path = Path(args.input)
        if not video_path.exists():
            sys.exit(f"Input file not found: {args.input}")
    else:
        # Download into a fresh temp directory so yt-dlp finds no pre-existing
        # file at the target path (which would cause it to skip).
        temp_dir = Path(tempfile.mkdtemp())
        video_path = temp_dir / "video.mp4"
        try:
            download_video(args.url, video_path)
        except Exception as e:
            shutil.rmtree(temp_dir, ignore_errors=True)
            sys.exit(f"Download failed: {e}")

    output_path = Path(args.output)
    frames = []

    try:
        print(f"Extracting frames at {args.fps} fps, {TARGET_W}x{TARGET_H} RGB24...")

        for i, frame_data in enumerate(extract_frames_rgb24(video_path, args.fps)):
            frames.append(frame_data)

            if (i + 1) % 100 == 0:
                print(f"  Processed {i + 1} frames...", end='\r', flush=True)

        frame_count = len(frames)
        print(f"\nTotal frames: {frame_count}")

        file_size = GLIM_HEADER_SIZE + frame_count * FRAME_BYTES
        print(f"Output size:  {file_size} bytes ({file_size / 1024:.1f} KB)")

        with open(output_path, 'wb') as f:
            write_glim_header(f, frame_count, args.fps)
            for frame_data in frames:
                f.write(frame_data)

        print(f"Written to {output_path}")
    finally:
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
