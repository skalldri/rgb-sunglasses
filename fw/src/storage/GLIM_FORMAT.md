# GLIM — Glasses LED Image Media

GLIM is a purpose-built single-file animation container for fixed-geometry LED displays.
The format is designed for embedded streaming: all frames are fixed-size, so any frame
can be reached with a single O(1) seek rather than scanning the file.

**Extension:** `.glim`  
**Byte order:** little-endian throughout.  
**Reference implementation:** `glim_decoder.{h,cpp}` (this directory).  
**Conversion tools:** `tools/convert_bad_apple.py` (video → mono), `tools/convert_video_to_glim.py` (video → RGB24, or LZ4-compressed RGB24 with `--lz4`), `tools/convert_gif_to_glim.py` (GIF → mono or RGB24), `tools/generate_nyan_cat_glim.py` (procedural RGB24).

---

## 1. File Layout

```
┌──────────────────────────────────┐
│  Header  (header_size bytes)     │
├──────────────────────────────────┤
│  [LZ4 index table]               │  present only for Lz4PerFrame / Lz4PerFrameRgb24
├──────────────────────────────────┤
│  Frame 0                         │
│  Frame 1                         │
│  …                               │
│  Frame N-1                       │
└──────────────────────────────────┘
```

Frame data begins at the absolute file offset stored in `frame_data_offset`.
For Raw and Rgb24 formats this immediately follows the header.
For LZ4 formats an index table sits between the header and the frame data
(see §4).

---

## 2. Header — Version 1

The v1 header is exactly **24 bytes**.

| Offset | Size | Type     | Field              | Description |
|--------|------|----------|--------------------|-------------|
| 0      | 4    | uint32   | `magic`            | Always `0x474C494D` (`GLIM` in ASCII, bytes `47 4C 49 4D`). |
| 4      | 1    | uint8    | `version`          | Format version. v1 = `1`. |
| 5      | 1    | uint8    | `header_size`      | Total header length in bytes. v1 = `24`. |
| 6      | 1    | uint8    | `frame_format`     | Pixel encoding (see §3). |
| 7      | 1    | uint8    | `fps`              | Playback rate in frames per second. `0` is invalid; decoders clamp it to 24. |
| 8      | 2    | uint16   | `width`            | Display width in pixels. Must be > 0. |
| 10     | 2    | uint16   | `height`           | Display height in pixels. Must be > 0. |
| 12     | 4    | uint32   | `frame_count`      | Total number of frames in the file. |
| 16     | 4    | uint32   | `frame_data_offset`| Absolute byte offset from the start of the file to the first frame. |
| 20     | 1    | uint8    | `mono_color_r`     | Red component of the "on" pixel colour for mono formats (see §3.1). |
| 21     | 1    | uint8    | `mono_color_g`     | Green component. |
| 22     | 1    | uint8    | `mono_color_b`     | Blue component. |
| 23     | 1    | uint8    | *(reserved)*       | Must be `0`. Reserved for future use. |

### 2.1 Magic bytes

The magic value `0x474C494D` stored as a little-endian `uint32` occupies bytes
`47 4C 49 4D` on disk — the ASCII characters `G`, `L`, `I`, `M`.

### 2.2 Versioning and forward compatibility

A parser **must** read `version` and `header_size` from the first six bytes before
interpreting any other field.

| Condition | Action |
|-----------|--------|
| `version > MAX_SUPPORTED_VERSION` | Reject the file (return an error). |
| `header_size > sizeof(parsed_struct)` | The file has extra header fields unknown to this parser. Read up to the known size; skip remaining header bytes by seeking to `frame_data_offset`. |
| `header_size < 24` | File is truncated or corrupt. Reject. |

`frame_data_offset` is the authoritative pointer to frame data regardless of
header size. Future versions may extend the header freely as long as they update
both `header_size` and `frame_data_offset`.

### 2.3 Mono colour field (bytes 20–22)

Bytes 20–22 encode the RGB colour used for "on" pixels in the two monochrome
formats (Raw, Lz4PerFrame). The sentinel value `(0, 0, 0)` means **default
white** (`255, 255, 255`); this is the correct value to write for files produced
before this field was defined, preserving backward compatibility.

Explicit black (`1, 1, 1` or any other near-black) is representable, but a pure
`(0, 0, 0)` always decodes as white. Plan accordingly if you need very dark
mono colours.

This field is **ignored** for Rgb24 and Lz4PerFrameRgb24 — each pixel in those
formats carries its own colour.

---

## 3. Frame Formats

The `frame_format` byte selects the pixel encoding for every frame in the file.
All frames in a single file use the same format.

| Value | Name               | Pixels/byte | Bytes per frame          | Status |
|-------|--------------------|-------------|--------------------------|--------|
| `1`   | `Raw`              | 8 (1-bit)   | `⌈width × height / 8⌉`  | Implemented |
| `2`   | `Lz4PerFrame`      | 8 (1-bit)   | variable (LZ4)           | Decode implemented (no encoder tool) |
| `3`   | `Rgb24`            | ⅓ (24-bit)  | `width × height × 3`     | Implemented |
| `4`   | `Lz4PerFrameRgb24` | ⅓ (24-bit)  | variable (LZ4)           | Implemented |

Values `0` and `5`–`255` are undefined. Parsers must reject them.

### 3.1 Raw (format = 1)

One bit per pixel, **MSB-first**, **row-major** (left-to-right, top-to-bottom).

```
Pixel (x, y):
  bitIndex = y × width + x
  byte     = buf[bitIndex / 8]
  bit      = (byte >> (7 - bitIndex % 8)) & 1
```

Bit `1` means the pixel is "on" (rendered using the mono colour from the header,
§2.3). Bit `0` means "off" (black, not rendered).

**Frame size:** `⌈width × height / 8⌉` bytes. For the standard 40 × 12
display: `⌈480 / 8⌉ = 60 bytes`.

**Frame N is located at:** `frame_data_offset + N × frame_bytes`.

**Example** — 40 × 12 display, byte 0 of a frame:

```
Bit 7  Bit 6  Bit 5  Bit 4  Bit 3  Bit 2  Bit 1  Bit 0
(0,0)  (1,0)  (2,0)  (3,0)  (4,0)  (5,0)  (6,0)  (7,0)
```

### 3.2 Lz4PerFrame (format = 2)

Each frame is independently compressed with LZ4. A per-frame index table
(see §4) provides O(1) seek. The reference decoder handles this format via the
same code path as §3.4 (`readFrame()` → `readLz4Frame()` → `LZ4_decompress_safe`),
so a well-formed file decodes correctly; there is currently **no encoder tool**
that emits it (the converters produce mono Raw or `Lz4PerFrameRgb24`).

### 3.3 Rgb24 (format = 3)

Three bytes per pixel — R, G, B — in **row-major** order. No padding between
rows or pixels.

```
Pixel (x, y):
  offset = (y × width + x) × 3
  R = buf[offset + 0]
  G = buf[offset + 1]
  B = buf[offset + 2]
```

**Frame size:** `width × height × 3` bytes. For the standard 40 × 12 display:
`480 × 3 = 1 440 bytes`.

**Frame N is located at:** `frame_data_offset + N × frame_bytes` — identical
seeking arithmetic to Raw.

The mono colour header field (§2.3) is **ignored** for this format.

### 3.4 Lz4PerFrameRgb24 (format = 4)

LZ4-compressed Rgb24 with a per-frame index table (see §4). Each frame is
decompressed on read into the caller's uncompressed frame buffer. Produced by
`tools/convert_video_to_glim.py --lz4`. The decoder bounds the decompressed
frame at 40 × 12 × 3 = 1440 bytes (the panel geometry); an LZ4 file whose frame
exceeds that is rejected at `open()`.

---

## 4. LZ4 Index Table (formats 2 and 4)

> **Status:** implemented (`glim_decoder.cpp::readLz4Frame`, encoder in
> `convert_video_to_glim.py --lz4`).

For compressed formats, a contiguous table of `frame_count` unsigned 32-bit
integers sits between the header and the frame data:

```
┌──────────────────────────────────┐  ← byte header_size
│  index[0]  uint32  (4 bytes)     │  absolute file offset of frame 0
│  index[1]  uint32  (4 bytes)     │  absolute file offset of frame 1
│  …                               │
│  index[N-1] uint32 (4 bytes)     │
└──────────────────────────────────┘  ← byte frame_data_offset
```

`frame_data_offset` = `header_size + frame_count × 4`.

Each entry points to a compressed frame record:

```
┌──────────────────────────────────────┐  ← index[N]
│  compressed_size  uint16  (2 bytes)  │
├──────────────────────────────────────┤
│  LZ4 compressed data                 │  compressed_size bytes
└──────────────────────────────────────┘
```

The compressed payload decompresses to exactly one uncompressed frame
(`frame_bytes` bytes) using `LZ4_decompress_safe()`.

**Seek procedure:**

1. Read `index[N]` → absolute offset `pos`.
2. `fs_seek(file, pos, FS_SEEK_SET)`.
3. Read `uint16` → `compressed_size`.
4. Read `compressed_size` bytes.
5. `LZ4_decompress_safe(src, dst, compressed_size, frame_bytes)`.

---

## 5. Worked Examples

### 5.1 Raw file — 40 × 12, 24 fps, 5 258 frames, white pixels

```
Offset  Bytes        Value       Field
------  -----------  ----------  -------------------
0       47 4C 49 4D  0x474C494D  magic ("GLIM")
4       01           1           version
5       18           24          header_size
6       01           1           frame_format (Raw)
7       18           24          fps
8       28 00        40          width
10      0C 00        12          height
12      CA 14 00 00  5258        frame_count
16      18 00 00 00  24          frame_data_offset (= header_size; frames follow immediately)
20      00           0           mono_color_r  → decoder treats as 255 (white)
21      00           0           mono_color_g  → decoder treats as 255
22      00           0           mono_color_b  → decoder treats as 255
23      00           0           (reserved)
```

Total file size: `24 + 5 258 × 60 = 315 504 bytes` (~308 KB).

Frame seek: frame 100 at offset `24 + 100 × 60 = 6 024`.

### 5.2 Raw file — 40 × 12, 24 fps, custom red pixels

Same as §5.1 except bytes 20–22:

```
20      FF           255         mono_color_r
21      00           0           mono_color_g
22      00           0           mono_color_b
```

All "on" pixels are rendered red.

### 5.3 Rgb24 file — 40 × 12, 12 fps, 12 frames (Nyan Cat)

```
Offset  Bytes        Value       Field
------  -----------  ----------  -------------------
0       47 4C 49 4D  0x474C494D  magic
4       01           1           version
5       18           24          header_size
6       03           3           frame_format (Rgb24)
7       0C           12          fps
8       28 00        40          width
10      0C 00        12          height
12      0C 00 00 00  12          frame_count
16      18 00 00 00  24          frame_data_offset
20      00           0           mono_color_r  (ignored for Rgb24)
21      00           0           mono_color_g  (ignored)
22      00           0           mono_color_b  (ignored)
23      00           0           (reserved)
```

Total file size: `24 + 12 × 1 440 = 17 304 bytes` (~17 KB).

Frame seek: frame 5 at offset `24 + 5 × 1 440 = 7 224`.

---

## 6. Parser Validation Checklist

A conforming parser must reject any file where:

- `magic ≠ 0x474C494D`
- `version > MAX_SUPPORTED_VERSION` (current max = 1)
- `header_size < 24`
- `frame_format` is not one of `{1, 2, 3, 4}`
- `width == 0` or `height == 0`
- `fps == 0` (may warn and clamp rather than reject)

A conforming parser must tolerate:

- `header_size > 24` (future extension; skip unknown bytes via `frame_data_offset`)
- `mono_color_{r,g,b}` all zero (treat as white)
- `frame_count == 0` (valid empty file; no frames to play)

---

## 7. Design Rationale

**Why not GIF / WebP / APNG?**  
These codecs require complex decoders impractical on a resource-constrained
microcontroller. GLIM's fixed-size frames need only a multiply and a seek.

**Why a custom format at all?**  
Raw frame sequences (plain `.bin`) have no standard single-file container with
versioned metadata or fixed-size seeks. GLIM adds a minimal header that lets a
decoder validate the file, know the geometry, and jump to any frame in O(1)
without scanning.

**Why MSB-first for Raw?**  
Pixel (0, 0) is in the most-significant bit of byte 0. This matches the natural
left-to-right reading order when inspecting a hex dump and makes the encoding
self-evident.

**Why is `(0,0,0)` the default-white sentinel for mono colour?**  
Files produced before the mono colour field was introduced have `reserved = 0`
in bytes 20–23. Treating `(0,0,0)` as white means those files continue to
render correctly with no conversion required.

**Why is `frame_data_offset` explicit rather than computed?**  
Storing the offset explicitly lets future header versions add fields without
requiring old parsers to know the new header size. A parser that understands
only v1 can still skip an extended header and land on the correct frame data.
