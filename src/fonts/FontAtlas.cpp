#include <fonts/FontAtlas.h>

#include <fonts/Consolas.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(FontAtlas);

FontAtlas::FontAtlas() {
    static_assert(width == FontAtlas::atlasWidth);
    static_assert(height == FontAtlas::atlasHeight);

    unsigned char* data = header_data;

    // Load the font atlas into RAM
    for (size_t i = 0; i < atlasPixels; i++) {
        HEADER_PIXEL(data, atlasBuffer[i].rgb);
    }
}

void FontAtlas::DebugChar(char c) {
    if (c == ' ') {
        printk("SPACE");
    } else if (c >= atlasFirstChar && c <= atlasLastChar) {
        // Find the location in the atlas
        const size_t atlasOffset = c - atlasFirstChar;
        const size_t atlasStartingXLocation = atlasOffset * atlasPixelWidthPerChar;

        for (size_t y = 0; y < atlasHeight; y++) {
            for (size_t x = atlasStartingXLocation; x < (atlasStartingXLocation + atlasPixelWidthPerChar); x++) {
                // The atlas is RGB, but it's also white/black
                // We can just check the red color channel to see if the pixel is illuminated
                printk("%s", atlasBuffer[(y * atlasWidth) + x].rgb[0] == 0 ? " " : "*");
            }
            printk("\n");
        }
    }
    else {
        LOG_ERR("Char '%c' is not in the font atlas", c);
    }
    
}
