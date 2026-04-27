#pragma once

#include <animations/animation_base.h>

#include <animations/animation_types.h>

#include <zephyr/kernel.h>

// All services provide the BT "IsActive" service
template <class T, Animation A>
class BaseAnimationTemplate : public BaseAnimation
{
public:
    static constexpr Animation kAnimationId = A;
    static constexpr size_t kAnimationIdNum = (size_t)A;

    static T *getInstance()
    {
        static T anim;
        return &anim;
    }

    void setActive(bool active) override
    {
        if (sActiveStateObserver_)
        {
            sActiveStateObserver_->onActiveStateChanged(kAnimationId, active);
        }
    }

protected:
    BaseAnimationTemplate() {};
};