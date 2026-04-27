#pragma once

#include <animations/animation.h>

class NullAnimation : public BaseAnimationTemplate<NullAnimation, Animation::None>
{
public:
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;
};