#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

struct Pixel {
    uint8_t rgb[3];
};

// The callback function that the FontAtlas will execute when printing a character
using FontPrintFunc = std::function<void(size_t x, size_t y, bool filled)>;

class FontAtlas {
    public:
        FontAtlas();

        void DebugChar(char c);

        void PrintChar(char c, FontPrintFunc func);

        static constexpr size_t atlasHeight = 12;
        static constexpr size_t atlasWidth = 658;
        static constexpr size_t atlasPixels = atlasWidth * atlasHeight;

        static constexpr char atlasFirstChar = '!';
        static constexpr char atlasLastChar = '~';
        static constexpr size_t atlasTotalChars = atlasLastChar - atlasFirstChar + 1;
        static constexpr size_t atlasPixelWidthPerChar = atlasWidth / atlasTotalChars;

    private:
        void PrintSpace(FontPrintFunc func);
        void PrintUnsupported(FontPrintFunc func);

        Pixel atlasBuffer[atlasPixels];
};