#include <animations/audio_animation.h>
#include <sound/sound.h>
#include <sound/audio_dsp.h>

namespace
{
    class SoundAnimationAudioSource : public AnimationAudioSource
    {
    public:
        /* Drain the message queue and cache the most recent frame.
         * Called once per animation tick so the animation sees a consistent snapshot. */
        void update() override
        {
            audio_analysis_result tmp;
            while (k_msgq_get(&audio_result_q, &tmp, K_NO_WAIT) == 0)
            {
                cache_ = tmp;
            }
        }

        float getBandEnergy(size_t band) const override
        {
            return (band < AUDIO_NUM_BANDS) ? cache_.band_energy[band] : 0.0f;
        }

        bool isBeat(size_t band) const override
        {
            return (band < AUDIO_NUM_BANDS) && cache_.beat[band];
        }

        size_t numBands() const override
        {
            return AUDIO_NUM_BANDS;
        }

    private:
        audio_analysis_result cache_ = {};
    };

    SoundAnimationAudioSource sSoundSource;
}

void audio_animation_bind_default_sound_dependencies()
{
    AudioAnimation::getInstance()->setAudioSource(sSoundSource);
}
