#include <storage/glim_decoder.h>
#include <zephyr/logging/log.h>
#include <cstring>
#include <lz4.h> // LZ4_decompress_safe (CONFIG_LZ4); same lib the TPS25750 patch uses

LOG_MODULE_REGISTER(glim_decoder, LOG_LEVEL_INF);

bool GlimDecoder::getPixel(const uint8_t *buf, size_t x, size_t y, size_t width)
{
    size_t bitIdx = y * width + x;
    return (buf[bitIdx / 8u] >> (7u - (bitIdx % 8u))) & 1u;
}

void GlimDecoder::getPixelRgb(const uint8_t *buf, size_t x, size_t y, size_t width,
                               uint8_t &r, uint8_t &g, uint8_t &b)
{
    size_t offset = (y * width + x) * 3u;
    r = buf[offset + 0u];
    g = buf[offset + 1u];
    b = buf[offset + 2u];
}

int GlimDecoder::open(const char *path)
{
    close(); // Ensure any previous file is closed

    fs_file_t_init(&file_);
    int rc = fs_open(&file_, path, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("Failed to open %s: %d", path, rc);
        return rc;
    }

    // Read the on-disk header (16 bytes min for version/header_size detection)
    uint8_t headerBuf[24] = {};
    ssize_t nread = fs_read(&file_, headerBuf, 24);
    if (nread < 16) {
        LOG_ERR("Failed to read header from %s: %d", path, (int)nread);
        fs_close(&file_);
        return -EBADF;
    }

    // Parse header
    uint32_t magic = *(uint32_t *)&headerBuf[0];
    if (magic != kMagic) {
        LOG_ERR("Bad magic: expected 0x%08x, got 0x%08x", kMagic, magic);
        fs_close(&file_);
        return -EBADF;
    }

    uint8_t version = headerBuf[4];
    if (version > kCurrentVersion) {
        LOG_ERR("Unsupported GLIM version: %u", version);
        fs_close(&file_);
        return -EBADF;
    }

    uint8_t headerSize = headerBuf[5];
    if (headerSize < kMinHeaderSize) {
        LOG_ERR("Header too small: %u < %u", headerSize, kMinHeaderSize);
        fs_close(&file_);
        return -EBADF;
    }

    uint8_t formatByte = headerBuf[6];
    uint8_t fps        = headerBuf[7];
    uint16_t width     = *(uint16_t *)&headerBuf[8];
    uint16_t height    = *(uint16_t *)&headerBuf[10];
    uint32_t frameCount = *(uint32_t *)&headerBuf[12];
    uint32_t frameDataOffset = *(uint32_t *)&headerBuf[16];

    if (width == 0 || height == 0) {
        LOG_ERR("Invalid dimensions: %ux%u", width, height);
        fs_close(&file_);
        return -EBADF;
    }

    if (formatByte != (uint8_t)FrameFormat::Raw &&
        formatByte != (uint8_t)FrameFormat::Lz4PerFrame &&
        formatByte != (uint8_t)FrameFormat::Rgb24 &&
        formatByte != (uint8_t)FrameFormat::Lz4PerFrameRgb24) {
        LOG_ERR("Unknown frame format: %u", formatByte);
        fs_close(&file_);
        return -EBADF;
    }

    if (fps == 0) {
        LOG_WRN("FPS is 0, clamping to 24");
        fps = 24;
    }

    // Mono color: bytes 20-22 (formerly reserved). (0,0,0) means default white.
    uint8_t monoR = headerBuf[20];
    uint8_t monoG = headerBuf[21];
    uint8_t monoB = headerBuf[22];
    if (monoR == 0u && monoG == 0u && monoB == 0u) {
        monoR = monoG = monoB = 255u;
    }

    // Store parsed header
    header_.version    = version;
    header_.format     = (FrameFormat)formatByte;
    header_.fps        = fps;
    header_.width      = width;
    header_.height     = height;
    header_.frameCount = frameCount;
    header_.monoColorR = monoR;
    header_.monoColorG = monoG;
    header_.monoColorB = monoB;

    frameDataOffset_ = frameDataOffset;
    if ((FrameFormat)formatByte == FrameFormat::Rgb24 ||
        (FrameFormat)formatByte == FrameFormat::Lz4PerFrameRgb24) {
        frameBytes_ = (size_t)width * (size_t)height * 3u;
    } else {
        frameBytes_ = ((size_t)width * (size_t)height + 7u) / 8u;
    }

    // LZ4 formats carry a uint32 per-frame index table starting right after the
    // header (GLIM_FORMAT.md §4); each entry is the absolute offset of that
    // frame's [uint16 size][LZ4 block] record. The decompress scratch is fixed,
    // so reject any file whose frame decompresses beyond that bound.
    if ((FrameFormat)formatByte == FrameFormat::Lz4PerFrame ||
        (FrameFormat)formatByte == FrameFormat::Lz4PerFrameRgb24) {
        if (frameBytes_ > kMaxLz4FrameBytes) {
            LOG_ERR("LZ4 frame too large: %zu > %zu", frameBytes_, kMaxLz4FrameBytes);
            fs_close(&file_);
            return -EBADF;
        }
        indexTableOffset_ = headerSize;
    }

    fileOpen_ = true;
    LOG_INF("GLIM opened: %ux%u, %u frames @ %u fps, format %u",
            width, height, frameCount, fps, (uint8_t)header_.format);

    return 0;
}

void GlimDecoder::close()
{
    if (fileOpen_) {
        fs_close(&file_);
        fileOpen_ = false;
    }
}

int GlimDecoder::readFrame(uint32_t index, uint8_t *buf, size_t bufSize)
{
    if (!fileOpen_) {
        LOG_ERR("readFrame called on closed file");
        return -EBADF;
    }

    if (index >= header_.frameCount) {
        LOG_ERR("Frame index out of range: %u >= %u", index, header_.frameCount);
        return -EINVAL;
    }

    if (bufSize < frameBytes_) {
        LOG_ERR("Buffer too small: %zu < %zu", bufSize, frameBytes_);
        return -ENOBUFS;
    }

    if (header_.format == FrameFormat::Raw || header_.format == FrameFormat::Rgb24) {
        // Both Raw and Rgb24 are contiguous byte sequences with O(1) seek.
        return readRawFrame(index, buf, bufSize);
    } else {
        // Lz4PerFrame / Lz4PerFrameRgb24: index-table lookup + per-frame decompress.
        return readLz4Frame(index, buf, bufSize);
    }
}

int GlimDecoder::readLz4Frame(uint32_t index, uint8_t *buf, size_t bufSize)
{
    // recordOffset (below) and compressedSize come straight from an untrusted file, as does
    // header_.frameCount that bounds `index`. This stays memory-safe regardless of their values:
    // an out-of-range seek/read fails cleanly (-EIO), the compressedSize check gates the fs_read
    // into lz4Scratch_, and LZ4_decompress_safe() is capped by bufSize — a corrupt file yields an
    // error or a wrong-but-bounded frame, never an out-of-bounds access. Compute the index-table
    // position in 64-bit so `index * 4` can't wrap size_t on a 32-bit target for a huge frameCount.
    // 1. Read this frame's record offset from the index table.
    off_t idxPos = (off_t)((uint64_t)indexTableOffset_ + (uint64_t)index * sizeof(uint32_t));
    int rc = fs_seek(&file_, idxPos, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("fs_seek (index) failed: %d", rc);
        return rc;
    }

    uint32_t recordOffset = 0;
    ssize_t nread = fs_read(&file_, &recordOffset, sizeof(recordOffset));
    if (nread != (ssize_t)sizeof(recordOffset)) {
        LOG_ERR("index read failed: expected %zu, got %d", sizeof(recordOffset), (int)nread);
        return (nread < 0) ? (int)nread : -EIO;
    }

    // 2. Seek to the record and read the uint16 compressed length.
    rc = fs_seek(&file_, (off_t)recordOffset, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("fs_seek (record) failed: %d", rc);
        return rc;
    }

    uint16_t compressedSize = 0;
    nread = fs_read(&file_, &compressedSize, sizeof(compressedSize));
    if (nread != (ssize_t)sizeof(compressedSize)) {
        LOG_ERR("compressed-size read failed: expected %zu, got %d",
                sizeof(compressedSize), (int)nread);
        return (nread < 0) ? (int)nread : -EIO;
    }

    if (compressedSize == 0u || compressedSize > sizeof(lz4Scratch_)) {
        LOG_ERR("bad compressed size: %u (scratch %zu)", compressedSize, sizeof(lz4Scratch_));
        return -EBADF;
    }

    // 3. Read the compressed payload into scratch.
    nread = fs_read(&file_, lz4Scratch_, compressedSize);
    if (nread != (ssize_t)compressedSize) {
        LOG_ERR("compressed read failed: expected %u, got %d", compressedSize, (int)nread);
        return (nread < 0) ? (int)nread : -EIO;
    }

    // 4. Decompress straight into the caller's frame buffer. bufSize was already
    //    checked >= frameBytes_ by readFrame().
    int decoded = LZ4_decompress_safe((const char *)lz4Scratch_, (char *)buf,
                                      (int)compressedSize, (int)bufSize);
    if (decoded < 0) {
        LOG_ERR("LZ4 decompress failed for frame %u: %d", index, decoded);
        return -EIO;
    }
    if ((size_t)decoded != frameBytes_) {
        LOG_ERR("LZ4 frame %u decoded %d bytes, expected %zu", index, decoded, frameBytes_);
        return -EIO;
    }

    return 0;
}

int GlimDecoder::readRawFrame(uint32_t index, uint8_t *buf, size_t bufSize)
{
    off_t offset = (off_t)(frameDataOffset_ + (size_t)index * frameBytes_);
    int rc = fs_seek(&file_, offset, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("fs_seek failed: %d", rc);
        return rc;
    }

    ssize_t nread = fs_read(&file_, buf, frameBytes_);
    if (nread != (ssize_t)frameBytes_) {
        LOG_ERR("fs_read failed: expected %zu, got %d", frameBytes_, (int)nread);
        return (nread < 0) ? (int)nread : -EIO;
    }

    return 0;
}
