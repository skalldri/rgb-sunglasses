#include <animations/beat_animation.h>

static constexpr size_t kMaxBands = 4;

/* How long (ms) a beat flash stays lit after the beat fires. */
static constexpr size_t kBeatHoldMs = 100;

void BeatAnimation::setAudioSource(AnimationAudioSource &source) {
    audioSource_ = &source;
}

void BeatAnimation::setColor(const AnimationUint32ParameterSource &color) {
    color_ = &color;
}

void BeatAnimation::init() {
    for (size_t b = 0; b < kMaxBands; b++) {
        beatHoldMs_[b] = 0;
    }
}

void BeatAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    if (!audioSource_ || !color_) {
        for (size_t x = 0; x < renderer.displayWidth(); x++) {
            for (size_t y = 0; y < renderer.displayHeight(); y++) {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
        return;
    }

    audioSource_->update();

    uint32_t color = color_->get();
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 0) & 0xFF;

    size_t numBands = audioSource_->numBands();
    if (numBands > kMaxBands) {
        numBands = kMaxBands;
    }

    /* Update per-band beat-hold timers. */
    for (size_t band = 0; band < numBands; band++) {
        if (audioSource_->isBeat(band)) {
            beatHoldMs_[band] = kBeatHoldMs;
        } else if (timeSinceLastTickMs >= beatHoldMs_[band]) {
            beatHoldMs_[band] = 0;
        } else {
            beatHoldMs_[band] -= timeSinceLastTickMs;
        }
    }

    size_t W = renderer.displayWidth();
    size_t H = renderer.displayHeight();

    /* Each band occupies an equal-width column strip across the display. */
    size_t stripWidth = (W + numBands - 1) / numBands;

    for (size_t x = 0; x < W; x++) {
        size_t band = x / stripWidth;
        if (band >= numBands) {
            band = numBands - 1;
        }
        bool lit = (beatHoldMs_[band] > 0);
        for (size_t y = 0; y < H; y++) {
            renderer.setPixel(x, y, lit ? r : 0, lit ? g : 0, lit ? b : 0);
        }
    }
}
