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

    enum class FrameFormat : uint8_t { Raw = 1, Lz4PerFrame = 2 };

    struct Header {
        uint8_t     version;
        FrameFormat format;
        uint8_t     fps;
        uint16_t    width;
        uint16_t    height;
        uint32_t    frameCount;
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

    // Pure static utility: extract a 1-bit pixel from a bitpacked frame buffer.
    // MSB-first, row-major. No file I/O; safe to call from tests.
    // Pixel (0, 0) is the top-left. Returns true if the pixel is "on", false if "off".
    static bool getPixel(const uint8_t *buf, size_t x, size_t y, size_t width);

private:
    static constexpr uint8_t  kMinHeaderSize  = 24u;
    static constexpr uint32_t kHeaderMagic    = 0x474C494Du; // Little-endian 'GLIM'

    struct fs_file_t file_;
    bool     fileOpen_        = false;
    Header   header_          = {};
    uint32_t frameDataOffset_ = 0;
    size_t   frameBytes_      = 0; // ceil(width*height/8)

    int readRawFrame(uint32_t index, uint8_t *buf, size_t bufSize);
};
