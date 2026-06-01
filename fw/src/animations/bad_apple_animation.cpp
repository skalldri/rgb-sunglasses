#include <animations/bad_apple_animation.h>
#include <fonts/FontAtlas.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

LOG_MODULE_REGISTER(bad_apple_animation, LOG_LEVEL_INF);

#define BAD_APPLE_FILE_PATH "/NAND:/bad_apple.glim"
#define BAD_APPLE_FILE_PATH_SHORT "/NAND:/BAD_AP~1.GLI"

void BadAppleAnimation::init() {
    currentFrame_ = 0;
    accumulatedMs_ = 0;
    inErrorState_ = false;
    errorScrollOffset_ = 0;
    errorScrollAccumMs_ = 0;
}

void BadAppleAnimation::setActive(bool active) {
    // Notify the registry observer (same pattern as BaseAnimationTemplate::setActive)
    if (sActiveStateObserver_) {
        sActiveStateObserver_->onActiveStateChanged(kAnimationId, active);
    }

    if (active) {
        init();
        int rc = decoder_.open(BAD_APPLE_FILE_PATH);
        if (rc < 0) {
            LOG_ERR("Failed to open %s: %d", BAD_APPLE_FILE_PATH, rc);
            rc = decoder_.open(BAD_APPLE_FILE_PATH_SHORT);
            if (rc < 0) {
                LOG_ERR("Failed to open %s: %d", BAD_APPLE_FILE_PATH_SHORT, rc);
                inErrorState_ = true;
            }
        }
    } else {
        decoder_.close();
        inErrorState_ = false;
    }
}

void BadAppleAnimation::tick(AnimationRenderer& renderer, size_t timeSinceLastTickMs) {
    if (inErrorState_) {
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    if (!decoder_.isOpen() || decoder_.header().frameCount == 0) {
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    // Advance playback clock
    accumulatedMs_ += timeSinceLastTickMs;
    const uint32_t msPerFrame = (decoder_.header().fps > 0) ? (1000u / decoder_.header().fps) : 41u;

    while (accumulatedMs_ >= msPerFrame) {
        accumulatedMs_ -= msPerFrame;
        currentFrame_++;
        if (currentFrame_ >= decoder_.header().frameCount) {
            currentFrame_ = 0;
        }
    }

    // Read frame
    int rc = decoder_.readFrame(currentFrame_, frameBuf_, sizeof(frameBuf_));
    if (rc < 0) {
        LOG_ERR("Failed to read frame %u: %d", currentFrame_, rc);
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    // Render frame: unpack bitpacked pixels using the color declared in the GLIM header.
    const uint8_t cr = decoder_.header().monoColorR;
    const uint8_t cg = decoder_.header().monoColorG;
    const uint8_t cb = decoder_.header().monoColorB;
    for (size_t y = 0; y < renderer.displayHeight(); y++) {
        for (size_t x = 0; x < renderer.displayWidth(); x++) {
            bool on = GlimDecoder::getPixel(frameBuf_, x, y, renderer.displayWidth());
            renderer.setPixel(x, y, on ? cr : 0u, on ? cg : 0u, on ? cb : 0u);
        }
    }
}

void BadAppleAnimation::renderError(AnimationRenderer& renderer, size_t timeSinceLastTickMs) {
    // Clear display
    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }

    // Advance scroll offset at the same rate as TextAnimation: 1 pixel per kErrorScrollStepMs
    errorScrollAccumMs_ += timeSinceLastTickMs;
    while (errorScrollAccumMs_ >= kErrorScrollStepMs) {
        errorScrollAccumMs_ -= kErrorScrollStepMs;
        errorScrollOffset_--;
    }

    // Render "NO FILE" text using FontAtlas, scrolling right-to-left
    FontAtlas* atlas = FontAtlas::getInstance();
    const char* msg = "NO FILE";

    for (size_t i = 0; msg[i]; i++) {
        char c = msg[i];
        int32_t charX =
            errorScrollOffset_ + static_cast<int32_t>(i * FontAtlas::atlasPixelWidthPerChar);

        // Only render if the character is (at least partially) visible on the display
        if (charX + static_cast<int32_t>(FontAtlas::atlasPixelWidthPerChar) >= 0 &&
            charX < static_cast<int32_t>(renderer.displayWidth())) {
            atlas->PrintChar(c, [&](size_t fontX, size_t fontY, bool filled) {
                int32_t screenX = charX + static_cast<int32_t>(fontX);
                if (screenX >= 0 && screenX < static_cast<int32_t>(renderer.displayWidth()) &&
                    fontY < renderer.displayHeight()) {
                    uint8_t brightness = filled ? 255u : 0u;
                    renderer.setPixel(screenX, fontY, brightness, brightness, brightness);
                }
            });
        }
    }
}
