#include <zephyr/ztest.h>
#include <storage/glim_decoder.h>
#include <cstring>

ZTEST_SUITE(glim_decoder, NULL, NULL, NULL, NULL, NULL);

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
