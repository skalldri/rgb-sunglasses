#pragma once

#include <animations/animation.h>
#include <storage/glim_decoder.h>
#include <cstdint>

class BadAppleAnimation : public BaseAnimationTemplate<BadAppleAnimation, Animation::BadApple>
{
public:
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;
    void setActive(bool active) override;

private:
    static constexpr size_t kFrameBytes = (40u * 12u + 7u) / 8u; // 60

    GlimDecoder decoder_;
    uint32_t    currentFrame_  = 0;
    uint32_t    accumulatedMs_ = 0;
    uint8_t     frameBuf_[kFrameBytes];

    // Matches TextAnimation's default step time: 1 pixel per 50 ms
    static constexpr uint32_t kErrorScrollStepMs = 50u;

    bool        inErrorState_          = false;
    int32_t     errorScrollOffset_     = 0;
    uint32_t    errorScrollAccumMs_    = 0;

    void renderError(AnimationRenderer &renderer, size_t timeSinceLastTickMs);
};

void bad_apple_animation_bind_default_bt_dependencies();
