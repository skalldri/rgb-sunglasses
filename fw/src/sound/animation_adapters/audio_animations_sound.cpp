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
     * Beats for the RandomOnBeat color mode are LATCHED here, at drain time (once
     * per drained frame), not read from cache_.beat[] — so it doesn't matter which
     * of the potentially-two update() calls in a tick (the Beat animation's own,
     * or SoundColorBeatSource's below) drains a beat-carrying frame, and a beat
     * flag persisting in cache_ across ticks can't cause repeated re-rolls. */
    void update() override {
        audio_analysis_result tmp;
        while (k_msgq_get(&audio_result_q, &tmp, K_NO_WAIT) == 0) {
            cache_ = tmp;
            if (tmp.beat[kColorBeatBand]) {
                pendingColorBeat_ = true;
            }
        }
    }

    bool consumePendingColorBeat() {
        const bool beat = pendingColorBeat_;
        pendingColorBeat_ = false;
        return beat;
    }

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
    bool pendingColorBeat_ = false;
};

SoundAnimationAudioSource sSoundSource;

/* Beat feed for the ColorModeSource resolvers (issue #259). Its own update()
 * call is harmless when the active animation already drained the queue this
 * tick — the latch above makes drain order irrelevant. Sharing one latch is
 * safe for the same reason sSoundSource itself is shared: only one animation
 * ticks at a time. */
class SoundColorBeatSource : public AnimationBeatSource {
   public:
    bool consumeBeat() override {
        sSoundSource.update();
        return sSoundSource.consumePendingColorBeat();
    }
};

SoundColorBeatSource sColorBeatSource;
}  // namespace

void color_mode_bind_default_beat_source() {
    ColorModeSource::setDefaultBeatSource(&sColorBeatSource);
}

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
