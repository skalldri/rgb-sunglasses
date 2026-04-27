#include <animations/bt_animations.h>
#include <zephyr/logging/log.h>
#include <fonts/FontAtlas.h>

LOG_MODULE_REGISTER(bt_anim, LOG_LEVEL_INF);

void BtAdvertisingAnimation::init() {
    currentCycleTimeMs = 0;
}

void BtAdvertisingAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {

    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > kFadeTimeMs) {
        currentCycleTimeMs = 0;
    }

    size_t currentBrightness = 0;

    if (currentCycleTimeMs < kFadeHalfTimeMs) {
        currentBrightness = kMinFade + (kFadeDistance * ((float)currentCycleTimeMs) / ((float)kFadeHalfTimeMs));
    } else {
        currentBrightness = kMaxFade - (kFadeDistance * ((float)(currentCycleTimeMs-kFadeHalfTimeMs)) / ((float)kFadeHalfTimeMs));
    }

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, currentBrightness);
        }
    }
}

void BtConnectingAnimation::init() {
    isBrightFlash = false;
    currentCycleTimeMs = 0;
}

void BtConnectingAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {

    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > kFlashSpeedMs) {
        isBrightFlash = !isBrightFlash;
        currentCycleTimeMs = 0;
    }

    size_t currentBrightness = 0;

    if (isBrightFlash) {
        currentBrightness = kMaxFlash;
    } else {
        currentBrightness = kMinFlash;
    }

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, currentBrightness);
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

void BtPairingAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }

    char msg[16];
    snprintk(msg, 16, "%06u", pairingCode);
    const size_t currentMessageLen = strlen(msg);

    const size_t displayEdgeBuffer = FontAtlas::atlasPixelWidthPerChar;
    const int32_t displayWindowLeftSide = -displayEdgeBuffer;
    const int32_t displayWindowRightSide = renderer.displayWidth() + displayEdgeBuffer;
    const int32_t currentTextOffsetRelativeToDisplay = currentTextOffset + renderer.displayWidth();

    size_t firstChar = 0;

    if (currentTextOffsetRelativeToDisplay < displayWindowLeftSide) {
        int32_t offsetRelativeToDisplayWindowLeftSide = currentTextOffsetRelativeToDisplay - displayWindowLeftSide;
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

    int32_t charWindowPos;

    auto lambda = [&](size_t x, size_t y, bool filled) {
        int32_t realX = x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)renderer.displayWidth()) {
            return;
        }

        if (filled) {
            renderer.setPixel(realX, y, 0, 0, kLetterBrightness);
        }
    };

    for (size_t i = firstChar; i < currentMessageLen; i++) {
        const int32_t currentCharPos = i * FontAtlas::atlasPixelWidthPerChar;
        charWindowPos = currentTextOffsetRelativeToDisplay + currentCharPos;

        if (charWindowPos >= displayWindowLeftSide && charWindowPos < displayWindowRightSide) {
            FontAtlas::getInstance()->PrintChar(msg[i], lambda);
        } else if (charWindowPos > displayWindowRightSide) {
            break;
        }
    }

    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > kStepTimeMs) {
        currentCycleTimeMs = 0;
        currentTextOffset--;
    }
}
