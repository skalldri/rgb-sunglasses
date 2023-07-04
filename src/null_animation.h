#pragma once

#include <animation.h>

class NullAnimation : public Animation
{
    public:
        void init() override;
        void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) override;
};