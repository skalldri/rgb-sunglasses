#pragma once

#include <animations/animation.h>

class BtAdvertisingAnimation : public BaseAnimationTemplate<BtAdvertisingAnimation, Animation::BtAdvertising>
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

    private:
        static constexpr size_t kMinFade = 10;
        static constexpr size_t kMaxFade = 100;
        static constexpr size_t kFadeDistance = kMaxFade - kMinFade;

        // The time required to fade up and then back down
        static constexpr size_t kFadeTimeMs = 1000;
        static constexpr size_t kFadeHalfTimeMs = kFadeTimeMs/2;

        // Current cycle time within the animation cycle
        size_t currentCycleTimeMs = 0;
};

class BtConnectingAnimation : public BaseAnimationTemplate<BtConnectingAnimation, Animation::BtConnecting>
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;

    private:
        static constexpr size_t kMinFlash = 10;
        static constexpr size_t kMaxFlash = 100;

        // The time spent flashing on each pulse type
        static constexpr size_t kFlashSpeedMs = 300;

        // The current animation state
        bool isBrightFlash = false;

        // Current cycle time within the animation cycle
        size_t currentCycleTimeMs = 0;
};