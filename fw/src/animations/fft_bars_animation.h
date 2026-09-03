#pragma once

#include <animations/animation.h>
#include <animations/animation_audio_source.h>
#include <animations/fft_bar_mapping.h>
#include <animations/fft_visualization_config_source.h>

class FftBarsAnimation : public BaseAnimationTemplate<FftBarsAnimation, Animation::FftBars> {
   public:
    /* Injected by src/sound/animation_adapters/audio_animations_sound.cpp */
    void setAudioSource(AnimationAudioSource &source);

    /* Injected by src/bluetooth/animation_adapters/fft_bars_animation_bt.cpp. Optional -
     * tick() falls back to the constexpr table defaults when unset. */
    void setConfigSource(FftVisualizationConfigSource &source);

    /* Back to the constexpr fallbacks. The animation is a singleton, so a config source
     * installed by one native_sim test would otherwise leak into the next. */
    void clearConfigSource();

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

    /* One ~32 ms analysis frame spanned ~3 render ticks at the pre-issue-#376
     * 11.1 ms render rate, and the per-tick EMA ran against a held energy value
     * in between — so the historical response was ~3 EMA steps toward each new
     * frame. Running 3 steps per NEW frame reproduces that response at any
     * render tick rate and keeps the persisted smoothingCoeff tunable's meaning
     * unchanged.
     *
     * Known approximation (PR #378 review round 9): the audio source is
     * last-frame-wins for bucket energies, so when one tick observes MULTIPLE
     * new frames, all of their deposited steps run toward the NEWEST frame's
     * energy — 2 frames give 6 steps toward E2 (a 0.88 weight at coeff 0.3)
     * instead of 3 toward E1 then 3 toward E2 (0.66), and E1 is discarded. At
     * 33 ms ticks against 32 ms frames roughly 1 tick in 32 (~1 Hz) delivers
     * two frames, so the bars get a slightly sharper snap on that cadence.
     * Accepted: the older energy is genuinely unrecoverable without per-frame
     * buffering, and capping the deposit at one frame per tick would starve
     * the pool during real catch-up. */
    static constexpr uint32_t kEmaStepsPerFrame = 3;

    /* The audio thread's analysis cadence. A hand copy of sound.cpp's
     * BLOCK_CAPTURE_TIME_MS — but NOT an approximation-tolerant one: the
     * proration only works because the withdrawal rate (kEmaStepsPerFrame per
     * kAudioFrameMs of wall time) matches the deposit rate (kEmaStepsPerFrame
     * per real frame). If the capture time changed without this, the pool
     * would drain fully on every arrival tick — silently reverting to the
     * exact burst behavior the proration exists to remove. The audio adapter
     * BUILD_ASSERTs the two equal (audio_animations_sound.cpp, PR #378 review
     * round 9); this header stays sound-free so the DI suite compiles without
     * the audio stack. */
    static constexpr uint32_t kAudioFrameMs = 32;

    /* audio_result_q holds at most this many frames, so a larger frameCount()
     * delta is a stall artifact or a counter wrap — cap the DELTA (before any
     * multiply: a post-multiply cap is bypassed by uint32 wrap, PR #378
     * review). Mirrors the msgq depth; BUILD_ASSERTed by the adapter too. */
    static constexpr uint32_t kMaxCatchupFrames = 4;

   private:
    AnimationAudioSource *audioSource_ = nullptr;
    FftVisualizationConfigSource *configSource_ = nullptr;

    /* Sized to hold the maximum number of display buckets we ever expect. */
    static constexpr size_t kMaxDisplayBuckets = 24;

    /* Per-bucket bar-height fraction the EMA is converging toward: the dB-window mapping
     * of the NEWEST analysis frame's bucket power (fft_bar_height), recomputed only when a
     * new frame has arrived — 20 log10f calls per 32 ms, not per render tick. */
    float target_[kMaxDisplayBuckets] = {};

    /* Per-bucket smoothed bar height in [0, 1] (exponential moving average of target_).
     * Smoothing the clamped height rather than the linear power is what makes attack and
     * release symmetric in dB and stops an over-ceiling spike from pinning the bar for
     * several steps after it has passed. */
    float height_[kMaxDisplayBuckets] = {};

    /* audioSource_->frameCount() at the last tick — the EMA advances per NEW
     * analysis frame, not per render tick (issue #376).
     *
     * Concurrency (PR #378 review round 6): init() resets this, pendingEmaSteps_,
     * prorateRemainderMs_, target_[] and height_[] on whatever thread called
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
     * height_[]. The adapter's atomic frameCount_
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
