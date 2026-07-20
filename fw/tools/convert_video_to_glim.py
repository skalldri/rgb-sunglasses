#!/usr/bin/env python3
"""
Convert a video (local file or URL) to GLIM format for the LED sunglasses display.

Unlike convert_bad_apple.py (monochrome only), this writes full-colour frames.
By default it emits uncompressed Rgb24 (format 3, see fw/src/storage/GLIM_FORMAT.md
section 3.3). Pass --lz4 to emit Lz4PerFrameRgb24 (format 4, section 3.4/§4): each
frame is independently LZ4-compressed and preceded by a uint32 index table for
O(1) seeks — much smaller on disk for long clips.

Usage:
  python convert_video_to_glim.py --url "https://..." --output animation.glim
  python convert_video_to_glim.py --input video.mp4 --output animation.glim --fps 24
  python convert_video_to_glim.py --input video.mp4 --output animation.glim --lz4
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
GLIM_FRAME_FORMAT_LZ4_RGB24 = 4


def write_glim_header(f, frame_count: int, fps: int, frame_format: int,
                      frame_data_offset: int) -> None:
    """Write a 24-byte GLIM v1 header to a file object."""
    header = (
        struct.pack('<I', GLIM_MAGIC) +
        struct.pack('<B', GLIM_VERSION) +
        struct.pack('<B', GLIM_HEADER_SIZE) +
        struct.pack('<B', frame_format) +
        struct.pack('<B', fps) +
        struct.pack('<H', TARGET_W) +
        struct.pack('<H', TARGET_H) +
        struct.pack('<I', frame_count) +
        struct.pack('<I', frame_data_offset) +
        struct.pack('<BBB', 0, 0, 0) +          # mono colour fields, ignored for Rgb24
        struct.pack('<B', 0)                    # reserved
    )
    assert len(header) == GLIM_HEADER_SIZE, f"Header size mismatch: {len(header)} != {GLIM_HEADER_SIZE}"
    f.write(header)


def compress_frame_lz4(frame_data: bytes) -> bytes:
    """LZ4-compress one raw frame into a bare LZ4 block (no 4-byte size prefix).

    store_size=False matches the firmware decoder, which reads the compressed
    length from the index/record header and calls LZ4_decompress_safe() with an
    explicit uncompressed capacity — so the stream must NOT carry lz4's own
    size prefix. See GLIM_FORMAT.md §4.
    """
    try:
        import lz4.block
    except ImportError:
        raise RuntimeError("python lz4 not found. Install with: pip install lz4")
    return lz4.block.compress(frame_data, mode='default', store_size=False)


def write_glim_lz4(f, frames, fps: int) -> int:
    """Write a full Lz4PerFrameRgb24 (format 4) file. Returns bytes written.

    Layout (GLIM_FORMAT.md §4):
      [24-byte header][index table: frame_count * uint32][per-frame records]
    Each record is [uint16 compressed_size][compressed_size bytes of LZ4 block].
    index[N] is the absolute file offset of frame N's record.
    """
    frame_count = len(frames)
    compressed = [compress_frame_lz4(fr) for fr in frames]

    for i, comp in enumerate(compressed):
        if len(comp) > 0xFFFF:
            raise RuntimeError(
                f"Frame {i} compressed to {len(comp)} bytes, exceeds uint16 record size")

    # Index table sits between the header and the first record.
    frame_data_offset = GLIM_HEADER_SIZE + frame_count * 4

    # Compute absolute offset of each record; a record is 2 bytes (uint16 size)
    # plus its compressed payload.
    offsets = []
    pos = frame_data_offset
    for comp in compressed:
        offsets.append(pos)
        pos += 2 + len(comp)

    write_glim_header(f, frame_count, fps, GLIM_FRAME_FORMAT_LZ4_RGB24, frame_data_offset)
    for off in offsets:
        f.write(struct.pack('<I', off))
    for comp in compressed:
        f.write(struct.pack('<H', len(comp)))
        f.write(comp)

    return pos  # total bytes written


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
    parser.add_argument("--lz4", action="store_true",
                        help="Emit LZ4-compressed frames (format 4, Lz4PerFrameRgb24)")

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

        uncompressed_size = GLIM_HEADER_SIZE + frame_count * FRAME_BYTES

        if args.lz4:
            with open(output_path, 'wb') as f:
                file_size = write_glim_lz4(f, frames, args.fps)
            ratio = (uncompressed_size / file_size) if file_size else 0.0
            print(f"Output size:  {file_size} bytes ({file_size / 1024:.1f} KB), "
                  f"LZ4 format 4 — {ratio:.2f}x smaller than "
                  f"{uncompressed_size / 1024:.1f} KB uncompressed")
        else:
            print(f"Output size:  {uncompressed_size} bytes ({uncompressed_size / 1024:.1f} KB)")
            with open(output_path, 'wb') as f:
                write_glim_header(f, frame_count, args.fps, GLIM_FRAME_FORMAT_RGB24,
                                  GLIM_HEADER_SIZE)
                for frame_data in frames:
                    f.write(frame_data)

        print(f"Written to {output_path}")
    finally:
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
