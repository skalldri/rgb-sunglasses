/*
 * Verifies that FontAtlas's compile-time bit-packed atlas (kAtlasBits in
 * FontAtlas.cpp) is behavior-identical to the original runtime decode it
 * replaced: a HEADER_PIXEL walk over fonts/Consolas.h into a 3-bytes-per-pixel
 * buffer, with a pixel considered lit iff its red channel is nonzero.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fonts/Consolas.h>
#include <fonts/FontAtlas.h>
#include <zephyr/ztest.h>

#include <array>
#include <cstring>

namespace {

// Reference decode: exactly the loop the old FontAtlas constructor ran at boot.
// One meaningful bit per pixel (red channel nonzero), stored unpacked here.
struct ReferenceAtlas {
    bool lit[FontAtlas::atlasPixels];

    ReferenceAtlas() {
        const unsigned char *data = header_data;
        for (size_t i = 0; i < FontAtlas::atlasPixels; i++) {
            unsigned char rgb[3];
            HEADER_PIXEL(data, rgb);
            lit[i] = rgb[0] != 0;
        }
    }
};

ReferenceAtlas sReference;

// Captures one PrintChar invocation as a dense pixel grid plus bounds checks.
struct CharCapture {
    bool filled[FontAtlas::atlasHeight][FontAtlas::atlasPixelWidthPerChar];
    bool touched[FontAtlas::atlasHeight][FontAtlas::atlasPixelWidthPerChar];

    CharCapture(char c) {
        memset(filled, 0, sizeof(filled));
        memset(touched, 0, sizeof(touched));
        FontAtlas::getInstance()->PrintChar(c, [this](size_t x, size_t y, bool f) {
            zassert_true(x < FontAtlas::atlasPixelWidthPerChar, "x %zu out of bounds", x);
            zassert_true(y < FontAtlas::atlasHeight, "y %zu out of bounds", y);
            filled[y][x] = f;
            touched[y][x] = true;
        });
    }
};

}  // namespace

ZTEST_SUITE(font_atlas_tests, NULL, NULL, NULL, NULL, NULL);

/* Every atlas-backed character must match the reference decode pixel-for-pixel. */
ZTEST(font_atlas_tests, test_all_atlas_chars_match_reference) {
    for (char c = FontAtlas::atlasFirstChar; c <= FontAtlas::atlasLastChar; c++) {
        CharCapture cap(c);
        const size_t atlasStartingXLocation =
            (c - FontAtlas::atlasFirstChar) * FontAtlas::atlasPixelWidthPerChar;

        for (size_t y = 0; y < FontAtlas::atlasHeight; y++) {
            for (size_t x = 0; x < FontAtlas::atlasPixelWidthPerChar; x++) {
                zassert_true(cap.touched[y][x], "char '%c': pixel (%zu,%zu) never emitted", c, x,
                             y);
                const bool expected =
                    sReference.lit[(y * FontAtlas::atlasWidth) + (x + atlasStartingXLocation)];
                zassert_equal(cap.filled[y][x], expected,
                              "char '%c': pixel (%zu,%zu) is %d, reference says %d", c, x, y,
                              cap.filled[y][x], expected);
            }
        }
    }
}

/* Space fills the full character cell with blanks. */
ZTEST(font_atlas_tests, test_space_is_blank) {
    CharCapture cap(' ');
    for (size_t y = 0; y < FontAtlas::atlasHeight; y++) {
        for (size_t x = 0; x < FontAtlas::atlasPixelWidthPerChar; x++) {
            zassert_true(cap.touched[y][x], "space: pixel (%zu,%zu) never emitted", x, y);
            zassert_false(cap.filled[y][x], "space: pixel (%zu,%zu) unexpectedly lit", x, y);
        }
    }
}

/* An out-of-range character renders the solid "unsupported" block: interior
 * filled, 1-pixel border untouched. */
ZTEST(font_atlas_tests, test_unsupported_char_block) {
    CharCapture cap('\t');
    for (size_t y = 0; y < FontAtlas::atlasHeight; y++) {
        for (size_t x = 0; x < FontAtlas::atlasPixelWidthPerChar; x++) {
            const bool interior = y >= 1 && y < FontAtlas::atlasHeight - 1 && x >= 1 &&
                                  x < FontAtlas::atlasPixelWidthPerChar - 1;
            zassert_equal(cap.touched[y][x], interior,
                          "unsupported: pixel (%zu,%zu) touched=%d, expected %d", x, y,
                          cap.touched[y][x], interior);
            if (interior) {
                zassert_true(cap.filled[y][x], "unsupported: interior pixel (%zu,%zu) not lit", x,
                             y);
            }
        }
    }
}

/* The atlas is a real font, not degenerate data: every printable character
 * must light at least one pixel. */
ZTEST(font_atlas_tests, test_atlas_not_degenerate) {
    for (char c = FontAtlas::atlasFirstChar; c <= FontAtlas::atlasLastChar; c++) {
        CharCapture cap(c);
        bool anyLit = false;
        for (size_t y = 0; y < FontAtlas::atlasHeight && !anyLit; y++) {
            for (size_t x = 0; x < FontAtlas::atlasPixelWidthPerChar && !anyLit; x++) {
                anyLit = cap.filled[y][x];
            }
        }
        zassert_true(anyLit, "char '%c' renders completely blank", c);
    }
}
