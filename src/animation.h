#pragma once

#include <led_config.h>

class Animation
{
public:
    virtual void init() = 0;
    virtual void tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) = 0;
};