#pragma once

#include <animations/animation.h>
#include <storage/glim_decoder.h>
#include <cstdint>

class NyanCatAnimation : public BaseAnimationTemplate<NyanCatAnimation, Animation::NyanCat>
{
public:
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;
    void setActive(bool active) override;

private:
    // 40 × 12 × 3 bytes (RGB24 format)
    static constexpr size_t kFrameBytes = 40u * 12u * 3u;

    // 1 pixel per 50 ms, matching TextAnimation's default scroll rate
    static constexpr uint32_t kErrorScrollStepMs = 50u;

    GlimDecoder decoder_;
    uint32_t    currentFrame_       = 0;
    uint32_t    accumulatedMs_      = 0;
    uint8_t     frameBuf_[kFrameBytes];

    bool        inErrorState_       = false;
    int32_t     errorScrollOffset_  = 0;
    uint32_t    errorScrollAccumMs_ = 0;

    void renderError(AnimationRenderer &renderer, size_t timeSinceLastTickMs);
};

void nyan_cat_animation_bind_default_bt_dependencies();
