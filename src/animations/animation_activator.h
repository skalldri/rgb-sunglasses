#pragma once
#include <animations/animation_types.h>

class AnimationActivator {
public:
    virtual ~AnimationActivator() = default;
    virtual void changeToAnimation(Animation id) = 0;
};
