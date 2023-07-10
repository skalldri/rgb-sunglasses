#pragma once

#include <led_config.h>
#include <led_controller.h>

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_cpf.h>
#include <bluetooth/animation_service.h>
#include <bluetooth/is_active_service.h>
#include <pattern_controller.h>
#include <animations/animation_types.h>

#include <zephyr/kernel.h>


class BaseAnimation
{
public:
    virtual void init() = 0;
    virtual void tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId) = 0;
    virtual void setActive(bool active) = 0;
};

// All services provide the BT "IsActive" service
template <class T, Animation A>
class BaseAnimationTemplate : public BaseAnimation, public IsActiveService<T>
{
public:
    static constexpr Animation kAnimationId = A;
    static constexpr size_t kAnimationIdNum = (size_t) A;

    static T *getInstance()
    {
        static T anim;
        return &anim;
    }

    // Pass the active state chance down into our IsActiveService()
    void setActive(bool active) override {
        T::getInstance()->setIsActiveState(active);
    }

    // Callback when our active state is changed remotely
    void onRemoteActiveChange(bool active) override {
        if (active) {
            // Got change to active state!
            pattern_controller_change_to_animation(kAnimationId);
        }
    }

protected:
    BaseAnimationTemplate(){};
};