#pragma once

#include <led_config.h>
#include <led_controller.h>

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_cpf.h>
#include <bluetooth/animation_service.h>
#include <bluetooth/is_active_service.h>

#include <zephyr/kernel.h>

// Defines the ID for each animation
enum class Animation
{
    None = 0,
    ZigZag = 1,
    Text = 2,
    BtAdvertising = 3,
    BtConnecting = 4
};

class BaseAnimation
{
public:
    virtual void init() = 0;
    virtual void tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId) = 0;
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

protected:
    BaseAnimationTemplate(){};
};