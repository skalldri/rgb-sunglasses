#pragma once

#include <animations/animation.h>

class ZigZagAnimation : public Animation
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

    private:
        static constexpr size_t stepTime = 100;

        size_t currentIndex = 0;

        // Current cycle time within the animation cycle
        size_t currentCycleTimeMs = 0;
};