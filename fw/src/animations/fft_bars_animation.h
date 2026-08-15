#pragma once

#include <animations/animation.h>
#include <animations/animation_audio_source.h>

/**
 * @brief Runtime-tunable spectrogram visualization parameters.
 *
 * Decouples fft_bars_animation.cpp from any concrete BT/Settings-backed implementation:
 * the native_sim ztest suite (fw/tests/animations/fft_bars_animation_di/) never calls
 * setConfigSource(), so tick() always falls back to the historical constexpr defaults.
 */
class FftVisualizationConfigSource {
   public:
    virtual ~FftVisualizationConfigSource() = default;

    /** EMA weight applied to the newest energy sample; 1-this is applied to history. */
    virtual float getSmoothingCoeff() const = 0;

    /** Maps mean bucket power to a bar-height fraction in [0, 1]. */
    virtual float getEnergyScale() const = 0;
};

class FftBarsAnimation : public BaseAnimationTemplate<FftBarsAnimation, Animation::FftBars> {
   public:
    /* Injected by src/sound/animation_adapters/audio_animations_sound.cpp */
    void setAudioSource(AnimationAudioSource &source);

    /* Injected by src/bluetooth/animation_adapters/fft_bars_animation_bt.cpp. Optional -
     * tick() falls back to the historical constexpr defaults when unset. */
    void setConfigSource(FftVisualizationConfigSource &source);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

   private:
    AnimationAudioSource *audioSource_ = nullptr;
    FftVisualizationConfigSource *configSource_ = nullptr;

    /* Per-bucket smoothed energy (exponential moving average).
     * Sized to hold the maximum number of display buckets we ever expect. */
    static constexpr size_t kMaxDisplayBuckets = 24;
    float smoothed_[kMaxDisplayBuckets] = {};

    /* audioSource_->frameCount() at the last tick — the EMA advances per NEW
     * analysis frame, not per render tick (issue #376).
     *
     * Concurrency (PR #378 review round 6): init() resets this, pendingEmaSteps_,
     * prorateRemainderMs_ and smoothed_[] on whatever thread called
     * pattern_controller_change_to_animation() (BT RX, shell, SMP workqueue),
     * while tick() read-modify-writes them on the render thread. Normally that's
     * sequenced — init() runs before currentAnimation flips, so the render thread
     * is still ticking the OLD animation — but re-activating the animation that
     * is already current (an Is Active re-write, a shuffle hop re-picking it)
     * runs init() concurrently with a possible in-flight tick() of the SAME
     * instance. That overlap is accepted unsynchronized: every field is an
     * aligned 32-bit word (no tearing on the M33 or native_sim), so the worst
     * case is a lost frame-count snapshot — a delta capped at kMaxCatchupFrames,
     * a few extra EMA steps — or one tick rendering from a half-reset
     * smoothed_[]. The adapter's atomic frameCount_
     * (audio_animations_sound.cpp) addresses this same window for the
     * cross-thread COUNTER read; it is not evidence the rest is synchronized. */
    uint32_t lastFrameCount_ = 0;

    /* EMA steps owed but not yet run, and the wall-time proration remainder:
     * arriving frames deposit steps into the pool; each tick withdraws up to
     * dt's worth (kEmaStepsPerFrame per kAudioFrameMs, remainder carried), so
     * a render rate faster than the analysis cadence spreads a frame's steps
     * across the ticks it spans instead of bursting them (PR #378 review). */
    uint32_t pendingEmaSteps_ = 0;
    uint32_t prorateRemainderMs_ = 0;
};

void fft_bars_animation_bind_default_sound_dependencies();
void fft_bars_animation_bind_default_bt_dependencies();
