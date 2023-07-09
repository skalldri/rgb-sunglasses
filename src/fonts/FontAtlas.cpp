#include <fonts/FontAtlas.h>

#include <fonts/Consolas.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(FontAtlas);

FontAtlas* FontAtlas::getInstance() {
    static FontAtlas atlas;
    return &atlas;
}

FontAtlas::FontAtlas()
{
    static_assert(width == FontAtlas::atlasWidth);
    static_assert(height == FontAtlas::atlasHeight);

    unsigned char *data = header_data;

    // Load the font atlas into RAM
    for (size_t i = 0; i < atlasPixels; i++)
    {
        HEADER_PIXEL(data, atlasBuffer[i].rgb);
    }
}

void FontAtlas::PrintSpace(FontPrintFunc func)
{
    // Simple routine that fills an entire atlas character with blanks
    for (size_t y = 0; y < atlasHeight; y++)
    {
        for (size_t x = 0; x < atlasPixelWidthPerChar; x++)
        {
            func(x, y, false);
        }
    }
}

void FontAtlas::PrintUnsupported(FontPrintFunc func)
{
    // Simple routine that fills almost the entire atlas character with solid
    // to represent an unsupported character
    for (size_t y = 1; y < (atlasHeight - 1); y++)
    {
        for (size_t x = 1; x < (atlasPixelWidthPerChar - 1); x++)
        {
            func(x, y, true);
        }
    }
}

void FontAtlas::PrintChar(char c, FontPrintFunc func)
{
    if (c == ' ')
    {
        PrintSpace(func);
    }
    else if (c >= atlasFirstChar && c <= atlasLastChar)
    {
        // Find the location in the atlas
        const size_t atlasOffset = c - atlasFirstChar;
        const size_t atlasStartingXLocation = atlasOffset * atlasPixelWidthPerChar;

        for (size_t y = 0; y < atlasHeight; y++)
        {
            for (size_t x = 0; x < atlasPixelWidthPerChar; x++)
            {
                // The atlas is RGB, but it's also white/black
                // We can just check the red color channel to see if the pixel is illuminated
                func(x, y, atlasBuffer[(y * atlasWidth) + (x + atlasStartingXLocation)].rgb[0] != 0);
            }
        }
    }
    else
    {
        PrintUnsupported(func);
    }
}

void FontAtlas::DebugChar(char c)
{
    size_t currentRow = 0;
    auto lambda = [&](size_t x, size_t y, bool filled) {
        if (y > currentRow) {
            printk("\n");
            currentRow = y;
        }

        printk("%s", filled ? "*" : " ");
    };
   
    PrintChar(c, lambda);
    printk("\n");
}
