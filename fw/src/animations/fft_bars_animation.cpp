#include <animations/fft_bars_animation.h>

/* Width of each rendered bar in display pixels. */
static constexpr size_t kBarWidthPx = 1;

/* Maps mean bucket power to a bar-height fraction in [0, 1].
 * Tune empirically: kEnergyScale=20 means energy=0.05 fills the display. */
static constexpr float kEnergyScale = 20.0f;

/* Exponential moving average: weight applied to the newest energy sample.
 * The complementary weight (1 - kSmoothingCoeff) is applied to the previous
 * smoothed value.  Lower = slower/smoother, higher = faster/more reactive. */
static constexpr float kSmoothingCoeff = 0.3f;

/* ── Gradient constants ──────────────────────────────────────────────────────
 * Traditional VU colours: green (bottom, silence) → orange → red (top, clip).
 * Each lit pixel is coloured by its absolute row position so the hue conveys
 * how close the bar is to clipping, independent of per-bar height. */
struct GradientStop {
    uint8_t r, g, b;
};

static constexpr GradientStop kColorGreen = {0, 255, 0};
static constexpr GradientStop kColorOrange = {255, 165, 0};
static constexpr GradientStop kColorRed = {255, 0, 0};

/* Fraction of the display height at which the gradient transitions from the
 * green→orange segment to the orange→red segment.  0.5 = halfway up. */
static constexpr float kGradientMidpoint = 0.5f;

/* Linearly interpolate a single colour channel. */
static uint8_t lerp_channel(uint8_t from, uint8_t to, float t) {
    return static_cast<uint8_t>(from + t * (static_cast<int>(to) - static_cast<int>(from)) + 0.5f);
}

/* Map a row-position fraction [0=bottom, 1=top] to an RGB gradient colour. */
static void gradient_color(float fraction, uint8_t &r, uint8_t &g, uint8_t &b) {
    GradientStop from, to;
    float t;

    if (fraction < kGradientMidpoint) {
        from = kColorGreen;
        to = kColorOrange;
        t = fraction / kGradientMidpoint;
    } else {
        from = kColorOrange;
        to = kColorRed;
        t = (fraction - kGradientMidpoint) / (1.0f - kGradientMidpoint);
    }

    r = lerp_channel(from.r, to.r, t);
    g = lerp_channel(from.g, to.g, t);
    b = lerp_channel(from.b, to.b, t);
}

/* Render a single bar column (one pixel wide) at display column `x`. */
static void render_bar_column(AnimationRenderer &renderer, size_t x, size_t H, size_t barHeight) {
    for (size_t y = 0; y < H; y++) {
        bool lit = (H > 0) && (y >= H - barHeight);
        if (lit) {
            float rowFraction =
                (H > 1) ? static_cast<float>(H - 1 - y) / static_cast<float>(H - 1) : 0.0f;
            uint8_t r, g, b;
            gradient_color(rowFraction, r, g, b);
            renderer.setPixel(x, y, r, g, b);
        } else {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }
}

void FftBarsAnimation::setAudioSource(AnimationAudioSource &source) {
    audioSource_ = &source;
}

void FftBarsAnimation::setConfigSource(FftVisualizationConfigSource &source) {
    configSource_ = &source;
}

/* One ~32 ms analysis frame spanned ~3 render ticks at the pre-issue-#376 11.1 ms
 * render rate, and the per-tick EMA ran against a held energy value in between —
 * so the historical response was ~3 EMA steps toward each new frame. Advancing
 * exactly 3 steps per NEW frame reproduces that response at any render tick rate
 * and keeps the persisted smoothingCoeff tunable's meaning unchanged. */
static constexpr uint32_t kEmaStepsPerFrame = 3;

/* audio_result_q holds at most a few frames; bound the catch-up after a stall so a
 * wrapped/large frame-count delta cannot spin the EMA loop. */
static constexpr uint32_t kMaxEmaStepsPerTick = 4 * kEmaStepsPerFrame;

void FftBarsAnimation::init() {
    for (size_t b = 0; b < kMaxDisplayBuckets; b++) {
        smoothed_[b] = 0.0f;
    }
    lastFrameCount_ = audioSource_ ? audioSource_->frameCount() : 0;
}

void FftBarsAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    /* Motion is driven by frameCount() deltas, not wall time — see kEmaStepsPerFrame. */
    ARG_UNUSED(timeSinceLastTickMs);

    size_t W = renderer.displayWidth();
    size_t H = renderer.displayHeight();

    if (!audioSource_) {
        for (size_t x = 0; x < W; x++) {
            for (size_t y = 0; y < H; y++) {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
        return;
    }

    audioSource_->update();

    const float smoothingCoeff =
        configSource_ ? configSource_->getSmoothingCoeff() : kSmoothingCoeff;
    const float energyScale = configSource_ ? configSource_->getEnergyScale() : kEnergyScale;

    size_t numBuckets = audioSource_->numDisplayBuckets();
    if (numBuckets > kMaxDisplayBuckets) {
        numBuckets = kMaxDisplayBuckets;
    }

    /* Update smoothed energies with an exponential moving average, one batch of
     * EMA steps per NEWLY-ARRIVED analysis frame (none when no frame arrived), so
     * the bars' responsiveness does not depend on the render tick rate. */
    const uint32_t frameCount = audioSource_->frameCount();
    uint32_t emaSteps = (frameCount - lastFrameCount_) * kEmaStepsPerFrame;
    lastFrameCount_ = frameCount;
    if (emaSteps > kMaxEmaStepsPerTick) {
        emaSteps = kMaxEmaStepsPerTick;
    }

    for (size_t bucket = 0; bucket < numBuckets; bucket++) {
        float energy = audioSource_->getDisplayBucketEnergy(bucket);
        for (uint32_t s = 0; s < emaSteps; s++) {
            smoothed_[bucket] =
                smoothingCoeff * energy + (1.0f - smoothingCoeff) * smoothed_[bucket];
        }
    }

    /* Mirrored layout: the left half of the display shows buckets low→high
     * (bucket 0 at the outer left edge, highest bucket at the centre-left).
     * The right half mirrors this symmetrically (highest bucket at centre-right,
     * bucket 0 at the outer right edge). */
    size_t halfWidth = W / 2;
    size_t bucketsPerHalf = halfWidth / kBarWidthPx;
    if (bucketsPerHalf > numBuckets) {
        bucketsPerHalf = numBuckets;
    }

    for (size_t bucket = 0; bucket < bucketsPerHalf; bucket++) {
        float fraction = smoothed_[bucket] * energyScale;
        if (fraction > 1.0f) {
            fraction = 1.0f;
        }
        if (fraction < 0.0f) {
            fraction = 0.0f;
        }

        size_t barHeight = static_cast<size_t>(fraction * static_cast<float>(H) + 0.5f);

        /* Left side: bucket 0 at x=0, increasing frequency toward the centre. */
        size_t leftStartX = bucket * kBarWidthPx;

        /* Right side: mirror — bucket 0 at x=W-kBarWidthPx, toward the centre. */
        size_t rightStartX = W - (bucket + 1) * kBarWidthPx;

        for (size_t bx = 0; bx < kBarWidthPx; bx++) {
            if (leftStartX + bx < W) {
                render_bar_column(renderer, leftStartX + bx, H, barHeight);
            }
            if (rightStartX + bx < W) {
                render_bar_column(renderer, rightStartX + bx, H, barHeight);
            }
        }
    }

    /* Black-fill any gap between the two halves (e.g. if bucketsPerHalf *
     * kBarWidthPx does not reach the centre exactly). */
    size_t leftEnd = bucketsPerHalf * kBarWidthPx;
    size_t rightStart = W - bucketsPerHalf * kBarWidthPx;
    for (size_t x = leftEnd; x < rightStart; x++) {
        for (size_t y = 0; y < H; y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }
}
