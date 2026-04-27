#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>

class RainbowAnimationDependencies
{
public:
    RainbowAnimationDependencies(const AnimationUint32ParameterSource &stepTimeMs, const AnimationUint32ParameterSource &rainbowWidthPix)
        : stepTimeMs(stepTimeMs), rainbowWidthPix(rainbowWidthPix)
    {
    }

    const AnimationUint32ParameterSource &stepTimeMs;
    const AnimationUint32ParameterSource &rainbowWidthPix;
};

class RainbowAnimation : public BaseAnimationTemplate<RainbowAnimation, Animation::Rainbow>
{
public:
    void setDependencies(const RainbowAnimationDependencies &deps);
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

private:
    const RainbowAnimationDependencies *deps_ = nullptr;
    size_t currentCycleTimeMs = 0;
    size_t currentRainbowStep = 0;
};

void rainbow_animation_bind_default_dependencies();