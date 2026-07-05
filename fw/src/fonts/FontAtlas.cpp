#include <fonts/Consolas.h>
#include <fonts/FontAtlas.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>

LOG_MODULE_REGISTER(FontAtlas);

namespace {

constexpr size_t kAtlasWords = (FontAtlas::atlasPixels + 31) / 32;

// Compile-time equivalent of the old constructor loop + HEADER_PIXEL: Consolas.h is the
// palette-index variant of the GIMP header export -- header_data holds one cmap index per
// pixel, and HEADER_PIXEL just looks the index up in header_data_cmap. The atlas is RGB,
// but it's also white/black, so a pixel is "lit" iff its palette entry's red channel is
// nonzero (PrintChar only ever consulted rgb[0]). Packing that single bit per pixel at
// compile time puts the whole atlas in rodata: zero RAM, zero boot-time decode.
consteval std::array<uint32_t, kAtlasWords> decodeAtlas() {
    static_assert(width == FontAtlas::atlasWidth);
    static_assert(height == FontAtlas::atlasHeight);

    std::array<uint32_t, kAtlasWords> bits{};
    for (size_t i = 0; i < FontAtlas::atlasPixels; i++) {
        if (header_data_cmap[header_data[i]][0] != 0) {
            bits[i / 32] |= uint32_t{1} << (i % 32);
        }
    }
    return bits;
}

constexpr auto kAtlasBits = decodeAtlas();

constexpr bool atlasBit(size_t i) { return (kAtlasBits[i / 32] >> (i % 32)) & 1u; }

}  // namespace

FontAtlas *FontAtlas::getInstance() {
    static FontAtlas atlas;
    return &atlas;
}

void FontAtlas::PrintSpace(FontPrintFunc func) {
    // Simple routine that fills an entire atlas character with blanks
    for (size_t y = 0; y < atlasHeight; y++) {
        for (size_t x = 0; x < atlasPixelWidthPerChar; x++) {
            func(x, y, false);
        }
    }
}

void FontAtlas::PrintUnsupported(FontPrintFunc func) {
    // Simple routine that fills almost the entire atlas character with solid
    // to represent an unsupported character
    for (size_t y = 1; y < (atlasHeight - 1); y++) {
        for (size_t x = 1; x < (atlasPixelWidthPerChar - 1); x++) {
            func(x, y, true);
        }
    }
}

void FontAtlas::PrintChar(char c, FontPrintFunc func) {
    if (c == ' ') {
        PrintSpace(func);
    } else if (c >= atlasFirstChar && c <= atlasLastChar) {
        // Find the location in the atlas
        const size_t atlasOffset = c - atlasFirstChar;
        const size_t atlasStartingXLocation = atlasOffset * atlasPixelWidthPerChar;

        for (size_t y = 0; y < atlasHeight; y++) {
            for (size_t x = 0; x < atlasPixelWidthPerChar; x++) {
                // One bit per pixel: set = illuminated (see decodeAtlas above)
                func(x, y, atlasBit((y * atlasWidth) + (x + atlasStartingXLocation)));
            }
        }
    } else {
        PrintUnsupported(func);
    }
}

void FontAtlas::DebugChar(char c) {
    /*
    size_t currentRow = 0;
    auto lambda = [&](size_t x, size_t y, bool filled)
    {
        if (y > currentRow)
        {
            printk("\n");
            currentRow = y;
        }

        printk("%s", filled ? "*" : " ");
    };

    PrintChar(c, lambda);
    printk("\n");
    */
}
