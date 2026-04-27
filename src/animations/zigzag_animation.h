#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>

class ZigZagAnimationDependencies
{
public:
    ZigZagAnimationDependencies(const AnimationUint32ParameterSource &stepTimeMs, const AnimationUint32ParameterSource &color)
        : stepTimeMs(stepTimeMs), color(color)
    {
    }

    const AnimationUint32ParameterSource &stepTimeMs;
    const AnimationUint32ParameterSource &color;
};

class ZigZagAnimation : public BaseAnimationTemplate<ZigZagAnimation, Animation::ZigZag>
{
public:
    void setDependencies(const ZigZagAnimationDependencies &deps);
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

private:
    const ZigZagAnimationDependencies *deps_ = nullptr;
    size_t currentIndex = 0;

    // Current cycle time within the animation cycle
    size_t currentCycleTimeMs = 0;
};

void zigzag_animation_bind_default_dependencies();