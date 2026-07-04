#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// The callback function that the FontAtlas will execute when printing a character
using FontPrintFunc = std::function<void(size_t x, size_t y, bool filled)>;

class FontAtlas {
   public:
    void DebugChar(char c);

    void PrintChar(char c, FontPrintFunc func);

    static FontAtlas* getInstance();

    static constexpr size_t atlasHeight = 12;
    static constexpr size_t atlasWidth = 658;
    static constexpr size_t atlasPixels = atlasWidth * atlasHeight;

    static constexpr char atlasFirstChar = '!';
    static constexpr char atlasLastChar = '~';
    static constexpr size_t atlasTotalChars = atlasLastChar - atlasFirstChar + 1;
    static constexpr size_t atlasPixelWidthPerChar = atlasWidth / atlasTotalChars;

   private:
    // The atlas itself is decoded from fonts/Consolas.h at compile time into a
    // 1-bit-per-pixel rodata table (see kAtlasBits in FontAtlas.cpp) -- the font is
    // monochrome, so the old 3-bytes-per-pixel RAM copy (23.7KB of .bss) stored only
    // 1 meaningful bit per pixel (issue #75 follow-up RAM pass). No per-instance
    // state remains.
    FontAtlas() = default;

    void PrintSpace(FontPrintFunc func);
    void PrintUnsupported(FontPrintFunc func);
};
