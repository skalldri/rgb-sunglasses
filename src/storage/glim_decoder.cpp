#include <storage/glim_decoder.h>
#include <zephyr/logging/log.h>
#include <cstring>

LOG_MODULE_REGISTER(glim_decoder, LOG_LEVEL_INF);

bool GlimDecoder::getPixel(const uint8_t *buf, size_t x, size_t y, size_t width)
{
    size_t bitIdx = y * width + x;
    return (buf[bitIdx / 8u] >> (7u - (bitIdx % 8u))) & 1u;
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

    if (formatByte != (uint8_t)FrameFormat::Raw && formatByte != (uint8_t)FrameFormat::Lz4PerFrame) {
        LOG_ERR("Unknown frame format: %u", formatByte);
        fs_close(&file_);
        return -EBADF;
    }

    if (fps == 0) {
        LOG_WRN("FPS is 0, clamping to 24");
        fps = 24;
    }

    // Store parsed header
    header_.version    = version;
    header_.format     = (FrameFormat)formatByte;
    header_.fps        = fps;
    header_.width      = width;
    header_.height     = height;
    header_.frameCount = frameCount;

    frameDataOffset_ = frameDataOffset;
    frameBytes_      = (width * height + 7u) / 8u;

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

    if (header_.format == FrameFormat::Raw) {
        return readRawFrame(index, buf, bufSize);
    } else {
        LOG_ERR("LZ4 format not yet supported");
        return -ENOTSUP;
    }
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
