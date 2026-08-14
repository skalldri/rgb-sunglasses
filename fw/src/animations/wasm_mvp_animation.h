#pragma once

#include <animations/animation.h>

#include <cstddef>
#include <cstdint>

class WasmMvpAnimation : public BaseAnimationTemplate<WasmMvpAnimation, Animation::WasmMvp> {
   public:
    void init() override;
    void setActive(bool active) override;
    void tick(AnimationRenderer& renderer, size_t timeSinceLastTickMs) override;

   private:
    void fill(AnimationRenderer& renderer, uint32_t color);

    uint32_t elapsedMs_ = 0;
    bool activationPending_ = false;
    bool ready_ = false;
};
