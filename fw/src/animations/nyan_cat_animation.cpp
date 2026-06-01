#include <animations/nyan_cat_animation.h>
#include <fonts/FontAtlas.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nyan_cat_animation, LOG_LEVEL_INF);

#define NYAN_CAT_FILE_PATH       "/NAND:/nyan_cat.glim"
#define NYAN_CAT_FILE_PATH_SHORT "/NAND:/NYAN_CA~1.GLI"

void NyanCatAnimation::init() {
    currentFrame_       = 0;
    accumulatedMs_      = 0;
    inErrorState_       = false;
    errorScrollOffset_  = 0;
    errorScrollAccumMs_ = 0;
}

void NyanCatAnimation::setActive(bool active) {
    if (sActiveStateObserver_) {
        sActiveStateObserver_->onActiveStateChanged(kAnimationId, active);
    }

    if (active) {
        init();
        int rc = decoder_.open(NYAN_CAT_FILE_PATH);
        if (rc < 0) {
            LOG_ERR("Failed to open %s: %d", NYAN_CAT_FILE_PATH, rc);
            rc = decoder_.open(NYAN_CAT_FILE_PATH_SHORT);
            if (rc < 0) {
                LOG_ERR("Failed to open %s: %d", NYAN_CAT_FILE_PATH_SHORT, rc);
                inErrorState_ = true;
            }
        }
    } else {
        decoder_.close();
        inErrorState_ = false;
    }
}

void NyanCatAnimation::tick(AnimationRenderer& renderer, size_t timeSinceLastTickMs) {
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
    const uint32_t msPerFrame = (decoder_.header().fps > 0) ? (1000u / decoder_.header().fps) : 83u;

    while (accumulatedMs_ >= msPerFrame) {
        accumulatedMs_ -= msPerFrame;
        currentFrame_++;
        if (currentFrame_ >= decoder_.header().frameCount) {
            currentFrame_ = 0;
        }
    }

    // Read frame (RGB24: 3 bytes per pixel)
    int rc = decoder_.readFrame(currentFrame_, frameBuf_, sizeof(frameBuf_));
    if (rc < 0) {
        LOG_ERR("Failed to read frame %u: %d", currentFrame_, rc);
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    // Render frame: extract RGB per pixel
    for (size_t y = 0; y < renderer.displayHeight(); y++) {
        for (size_t x = 0; x < renderer.displayWidth(); x++) {
            uint8_t r, g, b;
            GlimDecoder::getPixelRgb(frameBuf_, x, y, renderer.displayWidth(), r, g, b);
            renderer.setPixel(x, y, r, g, b);
        }
    }
}

void NyanCatAnimation::renderError(AnimationRenderer& renderer, size_t timeSinceLastTickMs) {
    // Clear display
    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }

    // Advance scroll at 1 pixel per kErrorScrollStepMs
    errorScrollAccumMs_ += timeSinceLastTickMs;
    while (errorScrollAccumMs_ >= kErrorScrollStepMs) {
        errorScrollAccumMs_ -= kErrorScrollStepMs;
        errorScrollOffset_--;
    }

    // Scroll "NO FILE" in rainbow colors (fitting for NyanCat)
    FontAtlas* atlas = FontAtlas::getInstance();
    const char* msg = "NO FILE";

    for (size_t i = 0; msg[i]; i++) {
        char c = msg[i];
        int32_t charX =
            errorScrollOffset_ + static_cast<int32_t>(i * FontAtlas::atlasPixelWidthPerChar);

        if (charX + static_cast<int32_t>(FontAtlas::atlasPixelWidthPerChar) >= 0 &&
            charX < static_cast<int32_t>(renderer.displayWidth())) {

            // Cycle through rainbow colors per character
            static constexpr uint8_t kRainbow[7][3] = {
                {255,   0,   0},  // red
                {255, 128,   0},  // orange
                {255, 255,   0},  // yellow
                {  0, 255,   0},  // green
                {  0,   0, 255},  // blue
                {128,   0, 255},  // indigo
                {255,   0, 255},  // violet
            };
            const uint8_t *col = kRainbow[i % 7];

            atlas->PrintChar(c, [&](size_t fontX, size_t fontY, bool filled) {
                int32_t screenX = charX + static_cast<int32_t>(fontX);
                if (screenX >= 0 && screenX < static_cast<int32_t>(renderer.displayWidth()) &&
                    fontY < renderer.displayHeight()) {
                    renderer.setPixel(screenX, fontY,
                                      filled ? col[0] : 0u,
                                      filled ? col[1] : 0u,
                                      filled ? col[2] : 0u);
                }
            });
        }
    }
}
