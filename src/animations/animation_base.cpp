#include <animations/animation_base.h>

AnimationActiveStateObserver *BaseAnimation::sActiveStateObserver_ = nullptr;

void BaseAnimation::registerActiveStateObserver(AnimationActiveStateObserver *observer)
{
    sActiveStateObserver_ = observer;
}
