#include <zephyr/ztest.h>
#include <storage/glim_decoder.h>
#include <cstring>
#include <zephyr/fs/fs.h>

extern "C" {
#include <ff.h>
}

ZTEST_SUITE(glim_decoder, NULL, NULL, NULL, NULL, NULL);

// ---------------------------------------------------------------------------
// getPixel — Raw (1-bit) format
// ---------------------------------------------------------------------------

ZTEST(glim_decoder, test_getpixel_all_zeros)
{
    uint8_t buf[60] = {};
    for (size_t x = 0; x < 40; x++) {
        for (size_t y = 0; y < 12; y++) {
            zassert_false(GlimDecoder::getPixel(buf, x, y, 40), "All-zero buffer should return false");
        }
    }
}

ZTEST(glim_decoder, test_getpixel_all_ones)
{
    uint8_t buf[60];
    memset(buf, 0xFF, 60);
    for (size_t x = 0; x < 40; x++) {
        for (size_t y = 0; y < 12; y++) {
            zassert_true(GlimDecoder::getPixel(buf, x, y, 40), "All-ones buffer should return true");
        }
    }
}

ZTEST(glim_decoder, test_getpixel_first_pixel)
{
    uint8_t buf[60] = {};
    buf[0] = 0x80;
    zassert_true(GlimDecoder::getPixel(buf, 0, 0, 40), "Pixel (0,0) should be true");
    zassert_false(GlimDecoder::getPixel(buf, 1, 0, 40), "Pixel (1,0) should be false");
}

ZTEST(glim_decoder, test_getpixel_last_pixel)
{
    uint8_t buf[60] = {};
    buf[59] = 0x01;
    zassert_true(GlimDecoder::getPixel(buf, 39, 11, 40), "Pixel (39,11) should be true");
    zassert_false(GlimDecoder::getPixel(buf, 38, 11, 40), "Pixel (38,11) should be false");
}

ZTEST(glim_decoder, test_getpixel_row_boundary)
{
    uint8_t buf[60] = {};
    buf[4] = 0x01;   // (39,0)
    buf[5] = 0x80;   // (0,1)
    zassert_true(GlimDecoder::getPixel(buf, 39, 0, 40), "Pixel (39,0) should be true");
    zassert_true(GlimDecoder::getPixel(buf, 0, 1, 40), "Pixel (0,1) should be true");
}

ZTEST(glim_decoder, test_getpixel_msb_first)
{
    uint8_t buf[60] = {};
    buf[0] = 0b10110100;
    zassert_true(GlimDecoder::getPixel(buf, 0, 0, 40), "Pixel 0 should be on");
    zassert_false(GlimDecoder::getPixel(buf, 1, 0, 40), "Pixel 1 should be off");
    zassert_true(GlimDecoder::getPixel(buf, 2, 0, 40), "Pixel 2 should be on");
    zassert_true(GlimDecoder::getPixel(buf, 3, 0, 40), "Pixel 3 should be on");
    zassert_false(GlimDecoder::getPixel(buf, 4, 0, 40), "Pixel 4 should be off");
}

// ---------------------------------------------------------------------------
// getPixelRgb — Rgb24 format
// ---------------------------------------------------------------------------

ZTEST(glim_decoder, test_getpixelrgb_all_zeros)
{
    uint8_t buf[40 * 12 * 3] = {};
    uint8_t r, g, b;
    for (size_t x = 0; x < 40; x++) {
        for (size_t y = 0; y < 12; y++) {
            GlimDecoder::getPixelRgb(buf, x, y, 40, r, g, b);
            zassert_equal(r, 0, "R should be 0 for zero buffer at (%d,%d)", x, y);
            zassert_equal(g, 0, "G should be 0 for zero buffer at (%d,%d)", x, y);
            zassert_equal(b, 0, "B should be 0 for zero buffer at (%d,%d)", x, y);
        }
    }
}

ZTEST(glim_decoder, test_getpixelrgb_first_pixel)
{
    uint8_t buf[40 * 12 * 3] = {};
    buf[0] = 255;  // R
    buf[1] = 128;  // G
    buf[2] = 42;   // B
    uint8_t r, g, b;
    GlimDecoder::getPixelRgb(buf, 0, 0, 40, r, g, b);
    zassert_equal(r, 255, "R at (0,0) should be 255");
    zassert_equal(g, 128, "G at (0,0) should be 128");
    zassert_equal(b, 42,  "B at (0,0) should be 42");
}

ZTEST(glim_decoder, test_getpixelrgb_second_pixel)
{
    uint8_t buf[40 * 12 * 3] = {};
    buf[3] = 10;   // R of pixel (1,0)
    buf[4] = 20;   // G
    buf[5] = 30;   // B
    uint8_t r, g, b;
    GlimDecoder::getPixelRgb(buf, 1, 0, 40, r, g, b);
    zassert_equal(r, 10, "R at (1,0) should be 10");
    zassert_equal(g, 20, "G at (1,0) should be 20");
    zassert_equal(b, 30, "B at (1,0) should be 30");
}

ZTEST(glim_decoder, test_getpixelrgb_last_pixel)
{
    /* Last pixel is (39, 11): index = 11*40 + 39 = 479; byte offset = 479*3 = 1437 */
    uint8_t buf[40 * 12 * 3] = {};
    buf[1437] = 11;
    buf[1438] = 22;
    buf[1439] = 33;
    uint8_t r, g, b;
    GlimDecoder::getPixelRgb(buf, 39, 11, 40, r, g, b);
    zassert_equal(r, 11, "R at (39,11) should be 11");
    zassert_equal(g, 22, "G at (39,11) should be 22");
    zassert_equal(b, 33, "B at (39,11) should be 33");
}

ZTEST(glim_decoder, test_getpixelrgb_second_row_first_pixel)
{
    /* First pixel of row 1 is (0,1): index = 1*40 + 0 = 40; byte offset = 40*3 = 120 */
    uint8_t buf[40 * 12 * 3] = {};
    buf[120] = 100;
    buf[121] = 150;
    buf[122] = 200;
    uint8_t r, g, b;
    GlimDecoder::getPixelRgb(buf, 0, 1, 40, r, g, b);
    zassert_equal(r, 100, "R at (0,1) should be 100");
    zassert_equal(g, 150, "G at (0,1) should be 150");
    zassert_equal(b, 200, "B at (0,1) should be 200");
}

ZTEST(glim_decoder, test_getpixelrgb_isolation)
{
    /* Setting pixel (5,3) must not affect pixel (4,3) or (6,3). */
    uint8_t buf[40 * 12 * 3] = {};
    size_t offset = (3u * 40u + 5u) * 3u;
    buf[offset]     = 77;
    buf[offset + 1] = 88;
    buf[offset + 2] = 99;

    uint8_t r, g, b;
    GlimDecoder::getPixelRgb(buf, 5, 3, 40, r, g, b);
    zassert_equal(r, 77, "R at (5,3)"); zassert_equal(g, 88, "G at (5,3)"); zassert_equal(b, 99, "B at (5,3)");

    GlimDecoder::getPixelRgb(buf, 4, 3, 40, r, g, b);
    zassert_equal(r, 0, "R at (4,3) should be 0"); zassert_equal(g, 0, "G at (4,3) should be 0");

    GlimDecoder::getPixelRgb(buf, 6, 3, 40, r, g, b);
    zassert_equal(r, 0, "R at (6,3) should be 0"); zassert_equal(g, 0, "G at (6,3) should be 0");
}

// ---------------------------------------------------------------------------
// open() — validation rejection tests (no filesystem required)
// ---------------------------------------------------------------------------

/*
 * Write a 24-byte GLIM header with given fields into a file and attempt to
 * open it with GlimDecoder. Returns the open() return code.
 */
static int open_from_header(GlimDecoder &dec,
                             const char *path,
                             uint32_t magic,
                             uint8_t version,
                             uint8_t header_size,
                             uint8_t fmt,
                             uint8_t fps,
                             uint16_t w, uint16_t h,
                             uint32_t frame_count,
                             uint8_t mr, uint8_t mg, uint8_t mb)
{
    uint8_t hdr[24] = {};
    hdr[0] = magic & 0xFF;
    hdr[1] = (magic >> 8)  & 0xFF;
    hdr[2] = (magic >> 16) & 0xFF;
    hdr[3] = (magic >> 24) & 0xFF;
    hdr[4] = version;
    hdr[5] = header_size;
    hdr[6] = fmt;
    hdr[7] = fps;
    hdr[8]  = w & 0xFF;  hdr[9]  = w >> 8;
    hdr[10] = h & 0xFF;  hdr[11] = h >> 8;
    hdr[12] = frame_count & 0xFF;
    hdr[13] = (frame_count >> 8)  & 0xFF;
    hdr[14] = (frame_count >> 16) & 0xFF;
    hdr[15] = (frame_count >> 24) & 0xFF;
    /* frame_data_offset = 24 */
    hdr[16] = 24;
    hdr[20] = mr; hdr[21] = mg; hdr[22] = mb;

    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    zassert_ok(rc, "setup: create %s failed: %d", path, rc);
    fs_write(&f, hdr, sizeof(hdr));
    fs_close(&f);

    return dec.open(path);
}

ZTEST(glim_decoder_io, test_open_bad_magic)
{
    GlimDecoder dec;
    int rc = open_from_header(dec, "/TEST:/t_magic.glim",
                              0xDEADBEEFu, 1, 24, 1, 24, 40, 12, 1, 0, 0, 0);
    zassert_true(rc < 0, "Bad magic should reject");
    zassert_false(dec.isOpen());
}

ZTEST(glim_decoder_io, test_open_unsupported_version)
{
    GlimDecoder dec;
    int rc = open_from_header(dec, "/TEST:/t_ver.glim",
                              0x474C494Du, 99, 24, 1, 24, 40, 12, 1, 0, 0, 0);
    zassert_true(rc < 0, "Unsupported version should reject");
    zassert_false(dec.isOpen());
}

ZTEST(glim_decoder_io, test_open_header_too_small)
{
    GlimDecoder dec;
    int rc = open_from_header(dec, "/TEST:/t_hdr.glim",
                              0x474C494Du, 1, 16, 1, 24, 40, 12, 1, 0, 0, 0);
    zassert_true(rc < 0, "Undersized header should reject");
    zassert_false(dec.isOpen());
}

ZTEST(glim_decoder_io, test_open_zero_width)
{
    GlimDecoder dec;
    int rc = open_from_header(dec, "/TEST:/t_w0.glim",
                              0x474C494Du, 1, 24, 1, 24, 0, 12, 1, 0, 0, 0);
    zassert_true(rc < 0, "Zero width should reject");
    zassert_false(dec.isOpen());
}

ZTEST(glim_decoder_io, test_open_zero_height)
{
    GlimDecoder dec;
    int rc = open_from_header(dec, "/TEST:/t_h0.glim",
                              0x474C494Du, 1, 24, 1, 24, 40, 0, 1, 0, 0, 0);
    zassert_true(rc < 0, "Zero height should reject");
    zassert_false(dec.isOpen());
}

ZTEST(glim_decoder_io, test_open_unknown_format)
{
    GlimDecoder dec;
    int rc = open_from_header(dec, "/TEST:/t_fmt.glim",
                              0x474C494Du, 1, 24, 99, 24, 40, 12, 1, 0, 0, 0);
    zassert_true(rc < 0, "Unknown format should reject");
    zassert_false(dec.isOpen());
}

// ---------------------------------------------------------------------------
// Filesystem-backed suite — open() success + readFrame()
// ---------------------------------------------------------------------------

static FATFS s_fat;
static struct fs_mount_t s_mnt = {
    .type      = FS_FATFS,
    .mnt_point = "/TEST:",
    .fs_data   = &s_fat,
};
static bool s_fs_ready = false;

static void *fs_setup(void)
{
    int rc = fs_mkfs(FS_FATFS, (uintptr_t)"TEST", NULL, 0);
    if (rc != 0) { return NULL; }
    rc = fs_mount(&s_mnt);
    if (rc != 0) { return NULL; }
    s_fs_ready = true;
    return &s_mnt;
}

static void fs_teardown(void *)
{
    if (s_fs_ready) {
        fs_unmount(&s_mnt);
        s_fs_ready = false;
    }
}

/*
 * Write a complete Raw GLIM to path:
 *   - header with given fields
 *   - frame 0: fill byte f0, frame 1: fill byte f1
 */
static void write_raw_glim(const char *path, uint16_t w, uint16_t h,
                            uint32_t nframes, uint8_t fps,
                            uint8_t mr, uint8_t mg, uint8_t mb,
                            uint8_t f0_fill, uint8_t f1_fill)
{
    uint32_t frame_bytes = ((uint32_t)w * h + 7u) / 8u;
    /* Magic 0x474C494D stored little-endian → bytes {0x4D, 0x49, 0x4C, 0x47} */
    uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47,            // magic (LE: 0x474C494D)
        1, 24,                              // version, header_size
        1, fps,                             // format=Raw, fps
        (uint8_t)(w), (uint8_t)(w >> 8),   // width
        (uint8_t)(h), (uint8_t)(h >> 8),   // height
        (uint8_t)(nframes),    (uint8_t)(nframes >> 8),
        (uint8_t)(nframes >> 16), (uint8_t)(nframes >> 24),
        24, 0, 0, 0,                        // frame_data_offset
        mr, mg, mb, 0,                      // mono_color, reserved
    };

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
    zassert_equal(fs_write(&f, hdr, 24), (ssize_t)24);

    uint8_t *frame = (uint8_t *)k_malloc(frame_bytes);
    zassert_not_null(frame, "k_malloc failed");

    memset(frame, f0_fill, frame_bytes);
    zassert_equal(fs_write(&f, frame, frame_bytes), (ssize_t)frame_bytes);
    if (nframes >= 2) {
        memset(frame, f1_fill, frame_bytes);
        zassert_equal(fs_write(&f, frame, frame_bytes), (ssize_t)frame_bytes);
    }

    k_free(frame);
    fs_close(&f);
}

/*
 * Write a complete Rgb24 GLIM to path:
 *   - frame 0: fill (r0,g0,b0), frame 1: fill (r1,g1,b1)
 */
static void write_rgb24_glim(const char *path, uint16_t w, uint16_t h,
                              uint32_t nframes, uint8_t fps,
                              uint8_t r0, uint8_t g0, uint8_t b0,
                              uint8_t r1, uint8_t g1, uint8_t b1)
{
    uint32_t frame_bytes = (uint32_t)w * h * 3u;
    /* Magic 0x474C494D stored little-endian → bytes {0x4D, 0x49, 0x4C, 0x47} */
    uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47,            // magic (LE: 0x474C494D)
        1, 24, 3, fps,
        (uint8_t)(w), (uint8_t)(w >> 8),
        (uint8_t)(h), (uint8_t)(h >> 8),
        (uint8_t)(nframes), (uint8_t)(nframes >> 8),
        (uint8_t)(nframes >> 16), (uint8_t)(nframes >> 24),
        24, 0, 0, 0,
        0, 0, 0, 0,
    };

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
    zassert_equal(fs_write(&f, hdr, 24), (ssize_t)24);

    uint8_t *frame = (uint8_t *)k_malloc(frame_bytes);
    zassert_not_null(frame, "k_malloc failed");

    /* Frame 0: uniform colour (r0, g0, b0) */
    for (uint32_t i = 0; i < frame_bytes; i += 3) {
        frame[i] = r0; frame[i+1] = g0; frame[i+2] = b0;
    }
    zassert_equal(fs_write(&f, frame, frame_bytes), (ssize_t)frame_bytes);

    if (nframes >= 2) {
        for (uint32_t i = 0; i < frame_bytes; i += 3) {
            frame[i] = r1; frame[i+1] = g1; frame[i+2] = b1;
        }
        zassert_equal(fs_write(&f, frame, frame_bytes), (ssize_t)frame_bytes);
    }

    k_free(frame);
    fs_close(&f);
}

ZTEST_SUITE(glim_decoder_io, NULL, fs_setup, NULL, NULL, fs_teardown);

/* open() returns 0 and isOpen() is true for a valid Raw file */
ZTEST(glim_decoder_io, test_open_raw_success)
{
    write_raw_glim("/TEST:/open_raw.glim", 40, 12, 2, 24, 255, 0, 0, 0x00, 0xFF);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/open_raw.glim"));
    zassert_true(dec.isOpen());
    dec.close();
}

/* Parsed header fields are correct for Raw file */
ZTEST(glim_decoder_io, test_open_raw_header_fields)
{
    write_raw_glim("/TEST:/hdr_raw.glim", 40, 12, 5, 30, 128, 64, 32, 0x00, 0xFF);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/hdr_raw.glim"));
    zassert_equal(dec.header().fps,        30u);
    zassert_equal(dec.header().width,      40u);
    zassert_equal(dec.header().height,     12u);
    zassert_equal(dec.header().frameCount, 5u);
    zassert_equal(dec.header().monoColorR, 128u);
    zassert_equal(dec.header().monoColorG, 64u);
    zassert_equal(dec.header().monoColorB, 32u);
    zassert_equal((uint8_t)dec.header().format, 1u);  // Raw
    dec.close();
}

/* mono_color (0,0,0) sentinel is promoted to white (255,255,255) */
ZTEST(glim_decoder_io, test_open_mono_color_sentinel_becomes_white)
{
    write_raw_glim("/TEST:/sentinel.glim", 40, 12, 1, 24, 0, 0, 0, 0x00, 0x00);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/sentinel.glim"));
    zassert_equal(dec.header().monoColorR, 255u);
    zassert_equal(dec.header().monoColorG, 255u);
    zassert_equal(dec.header().monoColorB, 255u);
    dec.close();
}

/* fps=0 in the file is clamped to 24 by the decoder */
ZTEST(glim_decoder_io, test_open_fps_zero_clamped_to_24)
{
    write_raw_glim("/TEST:/fps0.glim", 40, 12, 1, 0, 0, 0, 0, 0x00, 0x00);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/fps0.glim"));
    zassert_equal(dec.header().fps, 24u);
    dec.close();
}

/* open() succeeds for Rgb24 format */
ZTEST(glim_decoder_io, test_open_rgb24_success)
{
    write_rgb24_glim("/TEST:/open_rgb.glim", 40, 12, 2, 12,
                     10, 20, 30, 100, 150, 200);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/open_rgb.glim"));
    zassert_true(dec.isOpen());
    zassert_equal((uint8_t)dec.header().format, 3u);  // Rgb24
    dec.close();
}

/* close() makes isOpen() return false */
ZTEST(glim_decoder_io, test_close_resets_open_flag)
{
    write_raw_glim("/TEST:/close_t.glim", 40, 12, 1, 24, 0, 0, 0, 0xAA, 0xAA);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/close_t.glim"));
    zassert_true(dec.isOpen());
    dec.close();
    zassert_false(dec.isOpen());
}

/* readFrame() on a closed decoder returns -EBADF */
ZTEST(glim_decoder_io, test_readframe_closed_returns_ebadf)
{
    GlimDecoder dec;
    uint8_t buf[60];
    int rc = dec.readFrame(0, buf, sizeof(buf));
    zassert_equal(rc, -EBADF);
}

/* readFrame() with out-of-range index returns -EINVAL */
ZTEST(glim_decoder_io, test_readframe_index_out_of_range)
{
    write_raw_glim("/TEST:/oob.glim", 40, 12, 2, 24, 0, 0, 0, 0xAA, 0xBB);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/oob.glim"));
    uint8_t buf[60];
    zassert_equal(dec.readFrame(2, buf, sizeof(buf)), -EINVAL);
    dec.close();
}

/* readFrame() with buffer too small returns -ENOBUFS */
ZTEST(glim_decoder_io, test_readframe_buffer_too_small)
{
    write_raw_glim("/TEST:/small.glim", 40, 12, 1, 24, 0, 0, 0, 0x55, 0x55);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/small.glim"));
    uint8_t buf[10];
    zassert_equal(dec.readFrame(0, buf, sizeof(buf)), -ENOBUFS);
    dec.close();
}

/* readFrame() returns the correct data for a Raw frame */
ZTEST(glim_decoder_io, test_readframe_raw_frame0_all_zeros)
{
    write_raw_glim("/TEST:/rf0.glim", 40, 12, 2, 24, 255, 0, 0, 0x00, 0xFF);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/rf0.glim"));

    uint8_t buf[60] = {};
    memset(buf, 0xAA, sizeof(buf));  // poison
    zassert_ok(dec.readFrame(0, buf, sizeof(buf)));
    for (size_t i = 0; i < 60; i++) {
        zassert_equal(buf[i], 0x00, "Frame 0 byte %d should be 0x00", i);
    }
    dec.close();
}

/* readFrame() correctly seeks to a non-zero frame index */
ZTEST(glim_decoder_io, test_readframe_raw_frame1_all_ones)
{
    write_raw_glim("/TEST:/rf1.glim", 40, 12, 2, 24, 255, 0, 0, 0x00, 0xFF);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/rf1.glim"));

    uint8_t buf[60] = {};
    zassert_ok(dec.readFrame(1, buf, sizeof(buf)));
    for (size_t i = 0; i < 60; i++) {
        zassert_equal(buf[i], 0xFF, "Frame 1 byte %d should be 0xFF", i);
    }
    dec.close();
}

/* readFrame() works in sequence: frame 0 then frame 1 */
ZTEST(glim_decoder_io, test_readframe_raw_sequential)
{
    write_raw_glim("/TEST:/rfseq.glim", 40, 12, 2, 24, 255, 0, 0, 0x11, 0x22);
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/rfseq.glim"));

    uint8_t buf[60];
    zassert_ok(dec.readFrame(0, buf, sizeof(buf)));
    zassert_equal(buf[0], 0x11u);

    zassert_ok(dec.readFrame(1, buf, sizeof(buf)));
    zassert_equal(buf[0], 0x22u);

    /* seek back to frame 0 */
    zassert_ok(dec.readFrame(0, buf, sizeof(buf)));
    zassert_equal(buf[0], 0x11u);
    dec.close();
}

/* readFrame() returns correct RGB24 pixel data */
ZTEST(glim_decoder_io, test_readframe_rgb24_pixel_data)
{
    write_rgb24_glim("/TEST:/rgb_r.glim", 40, 12, 2, 12,
                     10, 20, 30,   // frame 0
                     100, 150, 200); // frame 1
    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/rgb_r.glim"));

    /* Frame size = 40*12*3 = 1440 bytes */
    uint8_t *buf = (uint8_t *)k_malloc(1440);
    zassert_not_null(buf, "k_malloc");

    zassert_ok(dec.readFrame(0, buf, 1440));
    uint8_t r, g, b;
    GlimDecoder::getPixelRgb(buf, 0, 0, 40, r, g, b);
    zassert_equal(r, 10u);
    zassert_equal(g, 20u);
    zassert_equal(b, 30u);
    /* Verify a pixel in the middle of the frame too */
    GlimDecoder::getPixelRgb(buf, 20, 6, 40, r, g, b);
    zassert_equal(r, 10u);

    zassert_ok(dec.readFrame(1, buf, 1440));
    GlimDecoder::getPixelRgb(buf, 0, 0, 40, r, g, b);
    zassert_equal(r, 100u);
    zassert_equal(g, 150u);
    zassert_equal(b, 200u);

    k_free(buf);
    dec.close();
}

/* Lz4PerFrame format returns -ENOTSUP (not implemented) */
ZTEST(glim_decoder_io, test_readframe_lz4_returns_enotsup)
{
    /* Write header with format=2 (Lz4PerFrame) — no real LZ4 data needed
     * because readFrame() returns -ENOTSUP before touching frame data. */
    /* Magic 0x474C494D stored little-endian → bytes {0x4D, 0x49, 0x4C, 0x47} */
    uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47, 1, 24, 2, 24,  // magic (LE), fmt=Lz4PerFrame
        40, 0, 12, 0,
        1, 0, 0, 0,   // 1 frame
        24, 0, 0, 0,  // frame_data_offset
        0, 0, 0, 0,
    };
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, "/TEST:/lz4.glim", FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
    fs_write(&f, hdr, 24);
    uint8_t dummy[60] = {};
    fs_write(&f, dummy, 60);
    fs_close(&f);

    GlimDecoder dec;
    zassert_ok(dec.open("/TEST:/lz4.glim"));
    uint8_t buf[60];
    zassert_equal(dec.readFrame(0, buf, sizeof(buf)), -ENOTSUP);
    dec.close();
}
