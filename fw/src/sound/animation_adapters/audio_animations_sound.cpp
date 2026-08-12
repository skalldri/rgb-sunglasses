#include <animations/animation_audio_source.h>
#include <animations/color_mode_source.h>
#include <sound/audio_dsp.h>
#include <sound/sound.h>

#if defined(CONFIG_ANIMATION_BEAT)
#include <animations/beat_animation.h>
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
#include <animations/fft_bars_animation.h>
#endif

#if defined(CONFIG_ANIMATION_PULSE)
#include <animations/pulse_animation.h>
#endif

namespace {
/* Band 0 (bass) drives random-on-beat color changes. The G byte of the color
 * value is reserved and could select the band in a follow-up without a wire
 * format change (issue #259). */
constexpr size_t kColorBeatBand = 0;


/* Single shared instance: only one audio animation is active at a time,
 * so a single drain-and-cache source serves both without double-reads. */
class SoundAnimationAudioSource : public AnimationAudioSource {
   public:
    /* Drain the message queue and cache the most recent frame.
     * Called once per animation tick so the animation sees a consistent snapshot.
     * Beats for the edge-triggered consumers are COUNTED here, at drain time (once
     * per drained frame), not read from cache_.beat[] — so it doesn't matter which
     * of the potentially-several update() calls in a tick (the Beat animation's own,
     * or the beat source below) drains a beat-carrying frame, and a beat flag
     * persisting in cache_ across ticks can't cause repeated re-rolls. */
    void update() override {
        audio_analysis_result tmp;
        while (k_msgq_get(&audio_result_q, &tmp, K_NO_WAIT) == 0) {
            cache_ = tmp;
            if (tmp.beat[kColorBeatBand]) {
                beatCount_++;
            }
        }
    }

    uint32_t beatCount() const { return beatCount_; }

    size_t numBands() const override { return AUDIO_NUM_BANDS; }

    float getBandEnergy(size_t band) const override {
        return (band < AUDIO_NUM_BANDS) ? cache_.band_energy[band] : 0.0f;
    }

    bool isBeat(size_t band) const override {
        return (band < AUDIO_NUM_BANDS) && cache_.beat[band];
    }

    size_t numDisplayBuckets() const override { return AUDIO_NUM_DISPLAY_BUCKETS; }

    float getDisplayBucketEnergy(size_t bucket) const override {
        return (bucket < AUDIO_NUM_DISPLAY_BUCKETS) ? cache_.display_bucket_energy[bucket] : 0.0f;
    }

   private:
    audio_analysis_result cache_ = {};
    uint32_t beatCount_ = 0;
};

SoundAnimationAudioSource sSoundSource;

/* Single beat feed shared by every consumer: the ColorModeSource resolvers
 * (issue #259) and Pulse's beat-sync envelope (issue #148). Its own update()
 * call is harmless when the active animation already drained the queue this
 * tick — counting at drain time makes drain order irrelevant.
 *
 * One object is correct here precisely BECAUSE it reports a count rather than
 * consuming a latch: the per-consumer state lives in each consumer's own
 * AnimationBeatCursor, so N consumers each observe every beat. Reintroducing a
 * consume-once latch here — even one split per consumer, as issue #148 first
 * did — reintroduces issue #344, because the consumer set is not fixed (an
 * extension may declare up to RGBX_MAX_PARAMS colour params, each resolving
 * through its own ColorModeSource). */
class SoundBeatSource : public AnimationBeatSource {
   public:
    uint32_t beatCount() override {
        sSoundSource.update();
        return sSoundSource.beatCount();
    }
};

SoundBeatSource sBeatSource;
}  // namespace

void color_mode_bind_default_beat_source() {
    ColorModeSource::setDefaultBeatSource(&sBeatSource);
}

#if defined(CONFIG_ANIMATION_PULSE)
void pulse_animation_bind_default_sound_dependencies() {
    PulseAnimation::getInstance()->setBeatSource(&sBeatSource);
}
#endif

#if defined(CONFIG_ANIMATION_BEAT)
void beat_animation_bind_default_sound_dependencies() {
    BeatAnimation::getInstance()->setAudioSource(sSoundSource);
}
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
void fft_bars_animation_bind_default_sound_dependencies() {
    FftBarsAnimation::getInstance()->setAudioSource(sSoundSource);
}
#endif

#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
/* Sandboxed extensions receive the same drain-and-cache audio snapshot via
 * rgbx_inputs (the host copies the band/bucket getters into the extension's
 * input block each tick — extensions never touch this source directly).
 * Sharing the single static source is safe for the same reason the two
 * built-in audio animations share it: only one animation ticks at a time. */
void extension_host_bind_default_sound_dependencies() {
    extension_host::set_audio_source(&sSoundSource);
}
#endif
