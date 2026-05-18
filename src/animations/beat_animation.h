#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>
#include <animations/animation_audio_source.h>

class BeatAnimation : public BaseAnimationTemplate<BeatAnimation, Animation::Beat>
{
public:
    /* Injected by src/sound/animation_adapters/audio_animations_sound.cpp */
    void setAudioSource(AnimationAudioSource &source);

    /* Injected by src/bluetooth/animation_adapters/beat_animation_bt.cpp */
    void setColor(const AnimationUint32ParameterSource &color);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

private:
    AnimationAudioSource *audioSource_ = nullptr;
    const AnimationUint32ParameterSource *color_ = nullptr;

    /* Per-band beat-hold countdown (ms): keeps the flash visible after a beat fires. */
    size_t beatHoldMs_[4] = {};
};

void beat_animation_bind_default_sound_dependencies();
void beat_animation_bind_default_bt_dependencies();
