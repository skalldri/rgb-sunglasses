#pragma once
#include <animations/animation_types.h>

class AnimationActiveStateObserver {
public:
    virtual ~AnimationActiveStateObserver() = default;
    virtual void onActiveStateChanged(Animation id, bool active) = 0;
};
