#pragma once

#include <animations/animation_active_state_observer.h>
#include <animations/animation_renderer.h>

class BaseAnimation
{
public:
    virtual void init() = 0;
    virtual void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) = 0;
    virtual void setActive(bool active) = 0;

    static void registerActiveStateObserver(AnimationActiveStateObserver *observer);

protected:
    static AnimationActiveStateObserver *sActiveStateObserver_;
};
