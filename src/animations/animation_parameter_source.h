#pragma once

#include <stdint.h>

class AnimationUint32ParameterSource
{
public:
    virtual ~AnimationUint32ParameterSource() = default;
    virtual uint32_t get() const = 0;
};
