#pragma once

#include <animations/animation.h>

class RainbowAnimation : public BaseAnimationTemplate<RainbowAnimation, Animation::Rainbow>
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

    private:
        size_t currentCycleTimeMs = 0;
        size_t currentRainbowStep = 0;
};