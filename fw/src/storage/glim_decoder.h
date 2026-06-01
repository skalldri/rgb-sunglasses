#pragma once

#include <zephyr/fs/fs.h>
#include <cstdint>

// GLIM decoder for reading bitpacked monochrome LED animation frames.
// GLIM = Glasses LED Image Media
class GlimDecoder
{
public:
    static constexpr uint32_t kMagic          = 0x474C494Du; // 'GLIM'
    static constexpr uint8_t  kCurrentVersion = 1u;

    enum class FrameFormat : uint8_t {
        Raw               = 1, // 1 bit/px, MSB-first bitpacked, ceil(w*h/8) bytes/frame
        Lz4PerFrame       = 2, // LZ4-compressed Raw, per-frame index table (not yet implemented)
        Rgb24             = 3, // 3 bytes/px (R,G,B), row-major, w*h*3 bytes/frame
        Lz4PerFrameRgb24  = 4, // LZ4-compressed Rgb24, per-frame index table (not yet implemented)
    };

    struct Header {
        uint8_t     version;
        FrameFormat format;
        uint8_t     fps;
        uint16_t    width;
        uint16_t    height;
        uint32_t    frameCount;
        // "On" pixel color for mono formats (Raw, Lz4PerFrame).
        // Stored in header bytes 20-22 (formerly reserved). (0,0,0) → white (255,255,255).
        // Ignored for Rgb24/Lz4PerFrameRgb24 — each pixel carries its own color.
        uint8_t     monoColorR;
        uint8_t     monoColorG;
        uint8_t     monoColorB;
    };

    // Open a .glim file and validate the header.
    // Returns 0 on success, negative errno on failure.
    int open(const char *path);

    // Close the file. Safe to call multiple times or if open() failed.
    void close();

    bool isOpen() const { return fileOpen_; }
    const Header &header() const { return header_; }

    // Read frame `index` into `buf` (must be >= ceil(width*height/8) bytes).
    // Returns 0 on success, negative on error.
    int readFrame(uint32_t index, uint8_t *buf, size_t bufSize);

    // Pure static utility: extract a 1-bit pixel from a Raw bitpacked frame buffer.
    // MSB-first, row-major. No file I/O; safe to call from tests.
    // Pixel (0, 0) is the top-left. Returns true if the pixel is "on", false if "off".
    static bool getPixel(const uint8_t *buf, size_t x, size_t y, size_t width);

    // Pure static utility: extract an RGB pixel from an Rgb24 frame buffer.
    // Row-major, 3 bytes per pixel (R, G, B). No file I/O; safe to call from tests.
    static void getPixelRgb(const uint8_t *buf, size_t x, size_t y, size_t width,
                            uint8_t &r, uint8_t &g, uint8_t &b);

private:
    static constexpr uint8_t  kMinHeaderSize  = 24u;
    static constexpr uint32_t kHeaderMagic    = 0x474C494Du; // Little-endian 'GLIM'

    struct fs_file_t file_;
    bool     fileOpen_        = false;
    Header   header_          = {};
    uint32_t frameDataOffset_ = 0;
    size_t   frameBytes_      = 0; // bytes per frame: ceil(w*h/8) for Raw, w*h*3 for Rgb24

    int readRawFrame(uint32_t index, uint8_t *buf, size_t bufSize);
};
