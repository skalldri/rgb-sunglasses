#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>
#include <animations/animation_audio_source.h>

class FftBarsAnimation : public BaseAnimationTemplate<FftBarsAnimation, Animation::FftBars>
{
public:
    /* Injected by src/sound/animation_adapters/audio_animations_sound.cpp */
    void setAudioSource(AnimationAudioSource &source);

    /* Injected by src/bluetooth/animation_adapters/fft_bars_animation_bt.cpp */
    void setColor(const AnimationUint32ParameterSource &color);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

private:
    AnimationAudioSource *audioSource_ = nullptr;
    const AnimationUint32ParameterSource *color_ = nullptr;

    /* Per-bucket smoothed energy (exponential moving average).
     * Sized to hold the maximum number of display buckets we ever expect. */
    static constexpr size_t kMaxDisplayBuckets = 16;
    float smoothed_[kMaxDisplayBuckets] = {};
};

void fft_bars_animation_bind_default_sound_dependencies();
void fft_bars_animation_bind_default_bt_dependencies();
