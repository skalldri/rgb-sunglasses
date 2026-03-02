#pragma once

#include <animations/animation_is_active_binding.h>

#include <bluetooth/bt_service_cpp.h>

/**
 * @brief Reusable BLE `Is Active` characteristic for a specific animation.
 *
 * Provides the common read/write/notify characteristic shape and forwards
 * remote writes into @ref AnimationIsActiveBinding for the selected animation.
 */
template <Animation tAnimationId>
class IsActiveCharacteristic : public BtGattAutoReadWriteNotifyCharacteristic<"Is Active", bool, false>
{
public:
    using Base = BtGattAutoReadWriteNotifyCharacteristic<"Is Active", bool, false>;
    using Base::operator=;

    /**
     * @brief Updates local characteristic state and emits notify when changed.
     *
     * @param active New local active state.
     */
    void setActive(bool active)
    {
        this->operator=(active);
    }

    /**
     * @brief Reacts to remote writes by requesting animation activation.
     *
     * @param active Value written by the remote BLE client.
     */
    void onWrite(const bool &active)
    {
        AnimationIsActiveBinding<tAnimationId>::onRemoteActiveChange(active);
    }
};
