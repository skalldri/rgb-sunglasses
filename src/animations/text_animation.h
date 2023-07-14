#pragma once

#include <animations/animation.h>

class TextAnimation : public BaseAnimationTemplate<TextAnimation, Animation::Text, BtServiceId::Text>
{
    public:
        static constexpr size_t kMaxMsgLen = 255;

        TextAnimation();

        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

    private:
        const char* getStringFromSlot(size_t slot);

        size_t getUpNext();

        char currentMessage[kMaxMsgLen]; 

        // Current cycle time within the animation cycle
        size_t currentCycleTimeMs = 0;

        int32_t currentTextOffset = 0;
};