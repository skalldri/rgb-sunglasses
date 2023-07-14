#pragma once

#include <animations/animation.h>

class ZigZagAnimation : public BaseAnimationTemplate<ZigZagAnimation, Animation::ZigZag, BtServiceId::ZigZag>
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

    private:
        size_t currentIndex = 0;

        // Current cycle time within the animation cycle
        size_t currentCycleTimeMs = 0;
};