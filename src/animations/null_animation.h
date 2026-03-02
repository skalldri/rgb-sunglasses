#pragma once

#include <animations/animation.h>

class NullAnimation : public BaseAnimationTemplate<NullAnimation, Animation::None>
{
public:
    void init() override;
    void tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId) override;
};