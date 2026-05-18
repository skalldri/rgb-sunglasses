#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>
#include <animations/animation_audio_source.h>

enum class AudioAnimationMode : uint32_t
{
    BeatColor = 0,
    FrequencyBars = 1,
};

class AudioAnimation : public BaseAnimationTemplate<AudioAnimation, Animation::Audio>
{
public:
    /* Injected by src/sound/animation_adapters/audio_animation_sound.cpp */
    void setAudioSource(AnimationAudioSource &source);

    /* Injected by src/bluetooth/animation_adapters/audio_animation_bt.cpp */
    void setBtParameters(const AnimationUint32ParameterSource &mode,
                         const AnimationUint32ParameterSource &color);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

private:
    void tickBeatColor(AnimationRenderer &renderer, size_t timeSinceLastTickMs);
    void tickFrequencyBars(AnimationRenderer &renderer);

    AnimationAudioSource *audioSource_ = nullptr;
    const AnimationUint32ParameterSource *mode_ = nullptr;
    const AnimationUint32ParameterSource *color_ = nullptr;

    /* Per-band smoothed energy for the bar graph (exponential moving average). */
    float smoothed_[4] = {};

    /* Per-band beat-hold countdown (ms): keeps the flash visible after a beat fires. */
    size_t beatHoldMs_[4] = {};
};

void audio_animation_bind_default_sound_dependencies();
void audio_animation_bind_default_bt_dependencies();
