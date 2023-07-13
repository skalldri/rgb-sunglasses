#include <animations/bt_animations.h>
#include <zephyr/logging/log.h>
#include <fonts/FontAtlas.h>

LOG_MODULE_REGISTER(bt_anim, LOG_LEVEL_INF);

void BtAdvertisingAnimation::init() {
    currentCycleTimeMs = 0;
}

void BtAdvertisingAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {

    // What should the current brightness be?
    currentCycleTimeMs += timeSinceLastTickMs;

    // Wrap around animation cycle
    if (currentCycleTimeMs > kFadeTimeMs) {
        currentCycleTimeMs = 0;
    }

    // Current brightness: 
    // if less than half of kFadeTimeMs, we are getting brighter
    // if more than half of kFadeTimeMS, we are getting dimmer
    size_t currentBrightness = 0;
    
    if (currentCycleTimeMs < kFadeHalfTimeMs) {
        currentBrightness = kMinFade + (kFadeDistance * ((float)currentCycleTimeMs) / ((float)kFadeHalfTimeMs));
    } else {
        currentBrightness = kMaxFade - (kFadeDistance * ((float)(currentCycleTimeMs-kFadeHalfTimeMs)) / ((float)kFadeHalfTimeMs));
    }

    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, currentBrightness);
        }
    }
}

void BtConnectingAnimation::init() {
    isBrightFlash = false;
    currentCycleTimeMs = 0;
}

void BtConnectingAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {

    // What should the current brightness be?
    currentCycleTimeMs += timeSinceLastTickMs;

    // Wrap around animation cycle
    if (currentCycleTimeMs > kFlashSpeedMs) {
        // Invert the state of what we are flashing
        isBrightFlash = !isBrightFlash;
        // Wrap around
        currentCycleTimeMs = 0;
    }

    size_t currentBrightness = 0;
    
    if (isBrightFlash) {
        currentBrightness = kMaxFlash;
    } else {
        currentBrightness = kMinFlash;
    }

    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, currentBrightness);
        }
    }
}

void BtPairingAnimation::setPairingCode(unsigned int code) {
    pairingCode = code;
}

void BtPairingAnimation::init() {
    currentTextOffset = 0;
    currentCycleTimeMs = 0;
}

void BtPairingAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {
    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

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
    char msg[16];
    snprintk(msg, 16, "%06u", pairingCode);
    const size_t currentMessageLen = strlen(msg);

    // The total "width" of the virtual texture that would contain the entire string
    // const size_t renderedStringWidth = currentMessageLen * FontAtlas::atlasPixelWidthPerChar;

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

    // If we have finished scrolling the code, go back to the beginning
    if (firstChar >= currentMessageLen) {
        currentTextOffset = 0;
        return;
    }

    int32_t charWindowPos;

    // This function gets called repeatedly to render to the display
    auto lambda = [&](size_t x, size_t y, bool filled) {
        int32_t realX =  x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)config->displayWidth) {
            // Bail early if this pixel is not on the display
            return;
        }

        if (filled) {
            set_pixel_in_framebuffer(config, realX, y, bufferId, 0, 0, 10);
        }
    };

    for (size_t i = firstChar; i < currentMessageLen; i++) {
        // Calculate the position of the current character within the virtual texture buffer
        const int32_t currentCharPos = i * FontAtlas::atlasPixelWidthPerChar;

        // Calculate where we would be rendering this character within the display window
        charWindowPos = currentTextOffsetRelativeToDisplay + currentCharPos;

        // If the character is within the virtual display buffer, lets render it
        if (charWindowPos >= displayWindowLeftSide && charWindowPos < displayWindowRightSide) {
            FontAtlas::getInstance()->PrintChar(msg[i], lambda);

            // Debugging
            // printk("%c", currentMessage[i]);
        } else if (charWindowPos > displayWindowRightSide) {
            // Early optimization: if we have found a character which is off the edge of the right side of the display window,
            // we can stop iterating since no more chars will ever need to be rendered
            break;
        }
    }

    // printk("\n");

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > kStepTimeMs) {
        currentCycleTimeMs = 0;
        currentTextOffset--; // Move text one pixel to the left
    }
}