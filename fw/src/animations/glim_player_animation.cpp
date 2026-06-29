#include <animations/glim_player_animation.h>
#include <fonts/FontAtlas.h>
#include <storage/glim_registry.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(glim_player_animation, LOG_LEVEL_INF);

void GlimPlayerAnimation::setDependencies(const GlimPlayerAnimationDependencies &deps) {
    deps_ = &deps;
}

void GlimPlayerAnimation::setButtonSource(AnimationButtonSource &source) {
    buttonSource_ = &source;
}

void GlimPlayerAnimation::init() {
    decoder_.close();
    openIndex_ = kInvalidIndex;
    currentFrame_ = 0;
    accumulatedMs_ = 0;
    inErrorState_ = false;
    finishedOnce_ = false;
    errorScrollOffset_ = 0;
    errorScrollAccumMs_ = 0;
}

void GlimPlayerAnimation::setActive(bool active) {
    if (sActiveStateObserver_) {
        sActiveStateObserver_->onActiveStateChanged(kAnimationId, active);
    }

    if (active) {
        init();
    } else {
        decoder_.close();
        inErrorState_ = false;
    }
}

void GlimPlayerAnimation::openCurrentFile(size_t index) {
    currentFrame_ = 0;
    accumulatedMs_ = 0;
    finishedOnce_ = false;

    char path[glim_registry::kMaxNameLen + 32];
    if (!glim_registry::full_path(index, path, sizeof(path))) {
        LOG_ERR("No GLIM file at registry index %zu", index);
        decoder_.close();
        openIndex_ = kInvalidIndex;
        inErrorState_ = true;
        return;
    }

    int rc = decoder_.open(path);
    if (rc < 0) {
        LOG_ERR("Failed to open %s: %d", path, rc);
        openIndex_ = kInvalidIndex;
        inErrorState_ = true;
        return;
    }

    openIndex_ = index;
    inErrorState_ = false;
}

void GlimPlayerAnimation::onClipFinished() {
    switch (deps_->loopModeSource.get()) {
        case GlimLoopMode::LoopOne:
            currentFrame_ = 0;
            break;

        case GlimLoopMode::StopAfterOne:
            currentFrame_ = decoder_.header().frameCount - 1;
            finishedOnce_ = true;
            break;

        case GlimLoopMode::PlayAll: {
            size_t count = glim_registry::count();
            if (count > 0) {
                size_t next = (openIndex_ + 1) % count;
                // Update the selection source first so its BLE characteristic (and notify)
                // reflects the auto-advance, then actually open the new file.
                deps_->selectionSource.setSelection(next);
                openCurrentFile(next);
            } else {
                inErrorState_ = true;
            }
            break;
        }
    }
}

void GlimPlayerAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    if (!deps_) {
        for (size_t x = 0; x < renderer.displayWidth(); x++) {
            for (size_t y = 0; y < renderer.displayHeight(); y++) {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
        return;
    }

    size_t count = glim_registry::count();

    if (buttonSource_) {
        buttonSource_->update();
        if (count > 0) {
            if (buttonSource_->wasPressed(kNextButtonId)) {
                size_t next = (deps_->selectionSource.currentIndex() + 1) % count;
                deps_->selectionSource.setSelection(next);
            } else if (buttonSource_->wasPressed(kPrevButtonId)) {
                size_t prev = (deps_->selectionSource.currentIndex() + count - 1) % count;
                deps_->selectionSource.setSelection(prev);
            }
        }
    }

    if (count == 0) {
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    // Pick up any selection change, whether from the button press above or a BLE write.
    size_t selected = deps_->selectionSource.currentIndex();
    if (selected != openIndex_) {
        openCurrentFile(selected);
    }

    if (inErrorState_ || !decoder_.isOpen() || decoder_.header().frameCount == 0) {
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    if (decoder_.header().width > renderer.displayWidth() ||
        decoder_.header().height > renderer.displayHeight()) {
        LOG_ERR("GLIM dimensions %ux%u exceed display %zux%zu", decoder_.header().width,
                decoder_.header().height, renderer.displayWidth(), renderer.displayHeight());
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    if (!finishedOnce_) {
        accumulatedMs_ += timeSinceLastTickMs;
        const uint32_t msPerFrame =
            (decoder_.header().fps > 0) ? (1000u / decoder_.header().fps) : 41u;

        while (accumulatedMs_ >= msPerFrame && !finishedOnce_) {
            accumulatedMs_ -= msPerFrame;
            currentFrame_++;
            if (currentFrame_ >= decoder_.header().frameCount) {
                onClipFinished();
                break;
            }
        }
    }

    int rc = decoder_.readFrame(currentFrame_, frameBuf_, sizeof(frameBuf_));
    if (rc < 0) {
        LOG_ERR("Failed to read frame %u: %d", currentFrame_, rc);
        inErrorState_ = true;
        renderError(renderer, timeSinceLastTickMs);
        return;
    }

    // The file's own width/height (already verified above to be <= the display's) are the
    // frame buffer's actual stride/extent - using renderer.displayWidth()/Height() instead
    // would misinterpret the buffer's layout whenever a file is smaller than the display,
    // reading past the decoded frame data. Pixels outside the file's declared dimensions
    // (when the file is smaller than the display) are rendered black.
    const size_t frameWidth = decoder_.header().width;
    const size_t frameHeight = decoder_.header().height;

    if (decoder_.header().format == GlimDecoder::FrameFormat::Rgb24) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            for (size_t x = 0; x < renderer.displayWidth(); x++) {
                if (x < frameWidth && y < frameHeight) {
                    uint8_t r, g, b;
                    GlimDecoder::getPixelRgb(frameBuf_, x, y, frameWidth, r, g, b);
                    renderer.setPixel(x, y, r, g, b);
                } else {
                    renderer.setPixel(x, y, 0, 0, 0);
                }
            }
        }
    } else {
        // FrameFormat::Raw: unpack bitpacked pixels using the color declared in the header.
        const uint8_t cr = decoder_.header().monoColorR;
        const uint8_t cg = decoder_.header().monoColorG;
        const uint8_t cb = decoder_.header().monoColorB;
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            for (size_t x = 0; x < renderer.displayWidth(); x++) {
                if (x < frameWidth && y < frameHeight) {
                    bool on = GlimDecoder::getPixel(frameBuf_, x, y, frameWidth);
                    renderer.setPixel(x, y, on ? cr : 0u, on ? cg : 0u, on ? cb : 0u);
                } else {
                    renderer.setPixel(x, y, 0, 0, 0);
                }
            }
        }
    }
}

void GlimPlayerAnimation::renderError(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
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

    // Scroll "NO FILE" in rainbow colors
    FontAtlas *atlas = FontAtlas::getInstance();
    const char *msg = "NO FILE";

    for (size_t i = 0; msg[i]; i++) {
        char c = msg[i];
        int32_t charX =
            errorScrollOffset_ + static_cast<int32_t>(i * FontAtlas::atlasPixelWidthPerChar);

        if (charX + static_cast<int32_t>(FontAtlas::atlasPixelWidthPerChar) >= 0 &&
            charX < static_cast<int32_t>(renderer.displayWidth())) {
            // Cycle through rainbow colors per character
            static constexpr uint8_t kRainbow[7][3] = {
                {255, 0, 0},      // red
                {255, 128, 0},    // orange
                {255, 255, 0},    // yellow
                {0, 255, 0},      // green
                {0, 0, 255},      // blue
                {128, 0, 255},    // indigo
                {255, 0, 255},    // violet
            };
            const uint8_t *col = kRainbow[i % 7];

            atlas->PrintChar(c, [&](size_t fontX, size_t fontY, bool filled) {
                int32_t screenX = charX + static_cast<int32_t>(fontX);
                if (screenX >= 0 && screenX < static_cast<int32_t>(renderer.displayWidth()) &&
                    fontY < renderer.displayHeight()) {
                    renderer.setPixel(screenX, fontY, filled ? col[0] : 0u, filled ? col[1] : 0u,
                                      filled ? col[2] : 0u);
                }
            });
        }
    }
}
