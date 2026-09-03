#include <animations/fft_bars_animation.h>
#include <sound/audio_param_table.h>

/* Width of each rendered bar in display pixels. */
static constexpr size_t kBarWidthPx = 1;

/* Fallbacks used when no FftVisualizationConfigSource is installed (the native_sim DI
 * suite, or a build without the BT-backed AudioConfig). Pinned to the table so the
 * BT-free path and a virgin board render identically; the mapping itself is documented
 * in fft_bar_mapping.h. */
static constexpr float kSmoothingCoeff = audioParamDefaultF<kAudioParamFftSmoothingCoeff>();
static constexpr float kEnergyScale = audioParamDefaultF<kAudioParamFftEnergyScale>();
static constexpr float kFloorDb = audioParamDefaultF<kAudioParamFftFloorDb>();
static constexpr float kRangeDb = audioParamDefaultF<kAudioParamFftRangeDb>();
static constexpr float kTiltDbPerOctave = audioParamDefaultF<kAudioParamFftTiltDbOct>();
static_assert(kEnergyScale == kFftBarEnergyScaleUnity,
              "audio/fft_energy_scale's default must be the 0 dB point of the legacy gain, "
              "or a virgin board renders with a gain offset");

/* ── Gradient constants ──────────────────────────────────────────────────────
 * Traditional VU colours: green (bottom, silence) → orange → red (top, clip).
 * Each lit pixel is coloured by its absolute row position so the hue conveys
 * how close the bar is to clipping, independent of per-bar height. With the dB
 * window's ceiling at 0 dB (E = 1.0 — see fft_bar_mapping.h) the red rows are
 * lit only when the AGC's attack path is about to turn the mic down, so the
 * colour keeps its meter meaning. */
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

void FftBarsAnimation::clearConfigSource() {
    configSource_ = nullptr;
}

/* kEmaStepsPerFrame / kAudioFrameMs / kMaxCatchupFrames moved to the class
 * (fft_bars_animation.h) so the audio adapter can BUILD_ASSERT them against the
 * sound pipeline's exported constants (PR #378 review round 9). */

/* Bound on the owed-steps pool, sized to the msgq depth. Worst-case tick cost:
 * 12 steps x 24 buckets = 288 float multiply-adds, ~2-3 us on this M33+FPU at
 * 128 MHz — noise against the 33.3 ms frame budget. The 20 log10f calls of the
 * mapping run once per NEW frame (~30 us per 32 ms), not per step. */
static constexpr uint32_t kMaxPendingEmaSteps =
    FftBarsAnimation::kMaxCatchupFrames * FftBarsAnimation::kEmaStepsPerFrame;

void FftBarsAnimation::init() {
    for (size_t b = 0; b < kMaxDisplayBuckets; b++) {
        target_[b] = 0.0f;
        height_[b] = 0.0f;
    }
    lastFrameCount_ = audioSource_ ? audioSource_->frameCount() : 0;
    pendingEmaSteps_ = 0;
    prorateRemainderMs_ = 0;
}

void FftBarsAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {

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
    const FftBarWindow window =
        configSource_ ? FftBarWindow{configSource_->getFloorDb(), configSource_->getRangeDb(),
                                     configSource_->getTiltDbPerOctave(),
                                     configSource_->getEnergyScale()}
                      : FftBarWindow{kFloorDb, kRangeDb, kTiltDbPerOctave, kEnergyScale};

    size_t numBuckets = audioSource_->numDisplayBuckets();
    if (numBuckets > kMaxDisplayBuckets) {
        numBuckets = kMaxDisplayBuckets;
    }

    /* Update smoothed heights with an exponential moving average driven by
     * NEWLY-ARRIVED analysis frames (no frames -> no movement), prorated over
     * wall time: arriving frames deposit kEmaStepsPerFrame steps into a pool,
     * and each tick withdraws up to dt's worth (carry-remainder). At the 30 Hz
     * default that is ~3 steps on the tick the frame arrives — unchanged — but
     * at a render rate faster than the analysis cadence (the still-supported
     * 90 Hz display=render=11100 setup) the frame's steps spread across the
     * ticks it spans instead of bursting on one and freezing on the rest
     * (PR #378 review). */
    const uint32_t frameCount = audioSource_->frameCount();
    uint32_t newFrames = frameCount - lastFrameCount_;
    lastFrameCount_ = frameCount;
    const bool haveNewFrame = newFrames != 0;
    if (newFrames > kMaxCatchupFrames) {
        newFrames = kMaxCatchupFrames;  /* delta capped BEFORE the multiply */
    }
    pendingEmaSteps_ += newFrames * kEmaStepsPerFrame;
    if (pendingEmaSteps_ > kMaxPendingEmaSteps) {
        pendingEmaSteps_ = kMaxPendingEmaSteps;
    }

    /* The EMA target is the dB-window mapping of the newest frame's power
     * (fft_bar_mapping.h). Recomputed only when a frame actually arrived: the
     * source is last-frame-wins, so between frames the target cannot change,
     * and this keeps the log10f cost per frame rather than per render tick. */
    if (haveNewFrame) {
        for (size_t bucket = 0; bucket < numBuckets; bucket++) {
            target_[bucket] =
                fft_bar_height(audioSource_->getDisplayBucketEnergy(bucket), bucket, window);
        }
    }

    prorateRemainderMs_ += kEmaStepsPerFrame * (uint32_t)timeSinceLastTickMs;
    const uint32_t allowance = prorateRemainderMs_ / kAudioFrameMs;
    prorateRemainderMs_ -= allowance * kAudioFrameMs;
    const uint32_t emaSteps = (pendingEmaSteps_ < allowance) ? pendingEmaSteps_ : allowance;
    pendingEmaSteps_ -= emaSteps;

    for (size_t bucket = 0; bucket < numBuckets; bucket++) {
        const float target = target_[bucket];
        for (uint32_t s = 0; s < emaSteps; s++) {
            height_[bucket] = smoothingCoeff * target + (1.0f - smoothingCoeff) * height_[bucket];
        }
    }

    /* Mirrored layout: the left half of the display shows buckets low→high
     * (bucket 0 at the outer left edge, highest bucket at the centre-left).
     * The right half mirrors this symmetrically (highest bucket at centre-right,
     * bucket 0 at the outer right edge).
     *
     * Known limitation on proto0 (accepted 2026-09-03): the nose cutout removes
     * the bottom 2/4/6 rows of the innermost columns (fw/src/led_config.h
     * kFrameLedsOnRow), so buckets 15–19 (1.9–3 kHz) are only visible once their
     * bar exceeds that height. The treble tilt helps; it does not remap them. */
    size_t halfWidth = W / 2;
    size_t bucketsPerHalf = halfWidth / kBarWidthPx;
    if (bucketsPerHalf > numBuckets) {
        bucketsPerHalf = numBuckets;
    }

    for (size_t bucket = 0; bucket < bucketsPerHalf; bucket++) {
        float fraction = height_[bucket];
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
