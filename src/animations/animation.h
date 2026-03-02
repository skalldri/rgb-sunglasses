#pragma once

#include <animations/animation_base.h>

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_cpf.h>
#include <bluetooth/bt_service.h>
#include <animations/animation_registry.h>
#include <pattern_controller.h>
#include <animations/animation_types.h>

#include <zephyr/kernel.h>

// All services provide the BT "IsActive" service
template <class T, Animation A, BtServiceId B>
class BaseAnimationTemplate : public BaseAnimation, public BtService<B>
{
public:
    static constexpr Animation kAnimationId = A;
    static constexpr size_t kAnimationIdNum = (size_t)A;

    static T *getInstance()
    {
        static T anim;
        return &anim;
    }

    // Pass the active state change down into our IsActiveCharacteristic()
    void setActive(bool active) override
    {
        animation_registry_set_is_active(kAnimationId, active);
    }

protected:
    BaseAnimationTemplate() {};
};