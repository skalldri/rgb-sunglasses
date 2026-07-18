#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>

class PulseAnimationDependencies {
   public:
    PulseAnimationDependencies(const AnimationUint32ParameterSource &color,
                                const AnimationUint32ParameterSource &periodMs)
        : color(color), periodMs(periodMs) {}

    const AnimationUint32ParameterSource &color;
    const AnimationUint32ParameterSource &periodMs;
};

class PulseAnimation : public BaseAnimationTemplate<PulseAnimation, Animation::Pulse> {
   public:
    void setDependencies(const PulseAnimationDependencies &deps);
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

   private:
    const PulseAnimationDependencies *deps_ = nullptr;

    // Position within the current breathing cycle, in ms; wraps at period_ms.
    size_t currentCycleTimeMs = 0;
};

void pulse_animation_bind_default_dependencies();
