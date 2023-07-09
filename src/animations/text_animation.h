#pragma once

#include <animations/animation.h>

class TextAnimation : public BaseAnimationTemplate<TextAnimation, Animation::Text>
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

        void pickStaticMessage(size_t msgId);

    private:
        static constexpr size_t stepTime = 100; // The time required to move the text over one pixel

        const char* currentMessage = NULL;

        // Current cycle time within the animation cycle
        size_t currentCycleTimeMs = 0;

        int32_t currentTextOffset = 0;
};