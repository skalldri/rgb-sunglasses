#pragma once

#include <animations/animation_types.h>

#include <bluetooth/bt_service.h>

#include <pattern_controller.h>

template <Animation tAnimationId, BtServiceId tBtServiceId>
class AnimationIsActiveBinding : public BtService<tBtServiceId>
{
public:
    using SetterCallback = void (*)(bool);

    static AnimationIsActiveBinding<tAnimationId, tBtServiceId> *getInstance()
    {
        static AnimationIsActiveBinding<tAnimationId, tBtServiceId> instance;
        return &instance;
    }

    static void registerSetter(SetterCallback setter)
    {
        getInstance()->setter_ = setter;
    }

    static void setLocalActiveState(bool active)
    {
        if (getInstance()->setter_)
        {
            getInstance()->setter_(active);
        }
    }

    static void onRemoteActiveChange(bool active)
    {
        if (active)
        {
            pattern_controller_change_to_animation(tAnimationId);
        }
    }

private:
    SetterCallback setter_ = nullptr;
};
