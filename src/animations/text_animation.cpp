#include <animations/text_animation.h>
#include <fonts/FontAtlas.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(text_anim, LOG_LEVEL_INF);

const char* kStaticMessages[] = {
    "LIFE IS MADE OF LITTLE MOMENTS LIKE THIS",
    "WE ARE ALL WE NEED",
    "SO LONG AND THANKS FOR ALL THE FISH",
    "ANJUNAFAM",
    "BREAKFAST ON MARS",
    "PUSH THE BUTTON",
    "BONSOIR, NE VOUS INQUIETEZ PAS",
    "DREAMS ARE MADE OF NIGHTS LIKE THIS",
    "LIVE FROM THE GORGE AMPHITHEATER, THIS IS ABGT WEEKENDER 2023",
    "ABOVE AND BEYOND",
    "PLEASE WELCOME, ABOVE AND BEYOND",
    "THIS LOVE, KILLS ME, THIS LOVE, KILLS ME",
    "IT'S THE SAME WAY DOWN",
    "GROUP THERAPY",
    "ILAN BLUESTONE",
    "LUTTRELL",
    "YOTTO",
    "TINLICKER",
    "FLOW STATE",
    "THE GORGE",
};

void TextAnimation::init() {
    currentCycleTimeMs = 0;
    currentTextOffset = 0;
    currentMessage = kStaticMessages[0];
}

void TextAnimation::pickStaticMessage(size_t msgId) {
    currentTextOffset = 0;
    currentMessage = kStaticMessages[msgId % ARRAY_SIZE(kStaticMessages)];
}

void TextAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {
    // We want to scroll arbitrarily long messages across the LED panel
    // we also want to do this as efficiently as possible.
    //
    // High level theory of operation:
    // We will consider the entire text string to be a single giant texture.
    // The width of the texture == strlen(str) * FontAtlas::atlasPixelWidthPerChar
    // 
    // The texture will start at the right side of the display: we apply an offset to the texture
    // render location, and then slowly reduce this offset (and eventually make it negative) to cause
    // the texture to scroll across the display.
    //
    // We consider the display to be a "window" looking into the texture. To save on compute, we will only
    // attempt to render characters which are within the window + some bounding box.

    // First, lets define some useful constants

    // Optimization to avoid repeatedly calculating the strlen() of currentMessage
    const size_t currentMessageLen = strlen(currentMessage);

    // The total "width" of the virtual texture that would contain the entire string
    const size_t renderedStringWidth = currentMessageLen * FontAtlas::atlasPixelWidthPerChar;

    // The size of the buffer on either side of the display where we will continue attempting to render
    // characters, which allows characters to partially slide onto the display, one pixel at a time
    const size_t displayEdgeBuffer = FontAtlas::atlasPixelWidthPerChar;

    // The edges of the "display window"
    const int32_t displayWindowLeftSide = -displayEdgeBuffer;
    const int32_t displayWindowRightSide = config->displayWidth + displayEdgeBuffer;

    // When we reset currentTextOffset to zero, the text will start off the edge of the display
    // but only just
    const int32_t currentTextOffsetRelativeToDisplay = currentTextOffset + config->displayWidth;

    // Predict the first character of the string that could fit on the display
    size_t firstChar = 0;

    // Until currentTextOffset goes negative, the first char to display will always be 0
    // When currentTextOffset is negative, the first character has fallen off the edge of the display
    if (currentTextOffsetRelativeToDisplay < displayWindowLeftSide) {
        // For every FontAtlas::atlasPixelWidthPerChar we are beyond the displayWindowLeftSide, 
        // we can start one character later
        int32_t offsetRelativeToDisplayWindowLeftSide = currentTextOffsetRelativeToDisplay - displayWindowLeftSide;

        // For each FontAtlas::atlasPixelWidthPerChar we are beyond the display left edge, we can drop a character
        // Rely on integer division to round down
        int32_t charsToDrop = offsetRelativeToDisplayWindowLeftSide / (int32_t)(-FontAtlas::atlasPixelWidthPerChar);

        if (charsToDrop < 0) {
            LOG_ERR("Chars to drop is negative unexpectedly: %d %d %d", currentTextOffsetRelativeToDisplay, offsetRelativeToDisplayWindowLeftSide, charsToDrop);
        } else {
            firstChar += charsToDrop;
        }
    }

    if (firstChar >= currentMessageLen) {
        currentTextOffset = 0;
        return;
    }

    for (size_t i = firstChar; i < currentMessageLen; i++) {
        // Calculate the position of the current character within the virtual texture buffer
        const int32_t currentCharPos = i * FontAtlas::atlasPixelWidthPerChar;

        // Calculate where we would be rendering this character within the display window
        int32_t charWindowPos = currentTextOffsetRelativeToDisplay + currentCharPos;

        // If the character is within the virtual display buffer, lets render it
        if (charWindowPos >= displayWindowLeftSide && charWindowPos < displayWindowRightSide) {
            // TODO: font atlas rendering
            printk("%c", currentMessage[i]);
        } else if (charWindowPos > displayWindowRightSide) {
            // Early optimization: if we have found a character which is off the edge of the right side of the display window,
            // we can stop iterating since no more chars will ever need to be rendered
            break;
        }
    }

    printk("\n");

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > stepTime) {
        currentCycleTimeMs = 0;
        currentTextOffset--; // Move text one pixel to the left
    }
}