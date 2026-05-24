#include <zephyr/ztest.h>
#include <storage/glim_decoder.h>
#include <cstring>

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
