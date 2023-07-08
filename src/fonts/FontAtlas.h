#pragma once

#include <cstdint>
#include <cstddef>

struct Pixel {
    uint8_t rgb[3];
};

class FontAtlas {
    public:
        FontAtlas();

        void DebugChar(char c);

    private:
        
        
        static constexpr size_t atlasHeight = 12;
        static constexpr size_t atlasWidth = 658;
        static constexpr size_t atlasPixels = atlasWidth * atlasHeight;

        static constexpr char atlasFirstChar = '!';
        static constexpr char atlasLastChar = '~';
        static constexpr size_t atlasTotalChars = atlasLastChar - atlasFirstChar + 1;
        static constexpr size_t atlasPixelWidthPerChar = atlasWidth / atlasTotalChars;

        Pixel atlasBuffer[atlasPixels];
};