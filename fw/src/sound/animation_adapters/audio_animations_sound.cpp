#include <animations/animation_audio_source.h>
#include <sound/audio_dsp.h>
#include <sound/sound.h>

#if defined(CONFIG_ANIMATION_BEAT)
#include <animations/beat_animation.h>
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
#include <animations/fft_bars_animation.h>
#endif

namespace {
/* Single shared instance: only one audio animation is active at a time,
 * so a single drain-and-cache source serves both without double-reads. */
class SoundAnimationAudioSource : public AnimationAudioSource {
   public:
    /* Drain the message queue and cache the most recent frame.
     * Called once per animation tick so the animation sees a consistent snapshot. */
    void update() override {
        audio_analysis_result tmp;
        while (k_msgq_get(&audio_result_q, &tmp, K_NO_WAIT) == 0) {
            cache_ = tmp;
        }
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
};

SoundAnimationAudioSource sSoundSource;
}  // namespace

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
