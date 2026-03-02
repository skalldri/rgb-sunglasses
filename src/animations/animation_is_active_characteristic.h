#pragma once

#include <animations/animation_is_active_binding.h>

#include <bluetooth/bt_service_cpp.h>

template <Animation tAnimationId>
class IsActiveCharacteristic : public BtGattAutoReadWriteNotifyCharacteristic<"Is Active", bool, false>
{
public:
    using Base = BtGattAutoReadWriteNotifyCharacteristic<"Is Active", bool, false>;
    using Base::operator=;

    void setActive(bool active)
    {
        this->operator=(active);
    }

    void onWrite(const bool &active)
    {
        AnimationIsActiveBinding<tAnimationId>::onRemoteActiveChange(active);
    }
};
