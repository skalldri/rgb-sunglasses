#pragma once

#include <animations/animation_types.h>

#include <bluetooth/bt_service.h>
#include <bluetooth/is_active_characteristic.h>

#include <pattern_controller.h>

template <Animation tAnimationId, BtServiceId tBtServiceId>
class AnimationIsActiveBinding : public BtService<tBtServiceId>, public IsActiveCharacteristic<AnimationIsActiveBinding<tAnimationId, tBtServiceId>>
{
public:
    static AnimationIsActiveBinding<tAnimationId, tBtServiceId> *getInstance()
    {
        static AnimationIsActiveBinding<tAnimationId, tBtServiceId> instance;
        return &instance;
    }

    static void setLocalActiveState(bool active)
    {
        getInstance()->setIsActiveState(active);
    }

    void onRemoteActiveChange(bool active) override
    {
        if (active)
        {
            pattern_controller_change_to_animation(tAnimationId);
        }
    }
};
