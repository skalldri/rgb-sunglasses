#pragma once

#include <animations/animation_is_active_binding.h>
#include <bluetooth/bt_service_cpp.h>

/**
 * @brief Reusable BLE `Is Active` characteristic for a specific animation.
 *
 * Provides the common read/write/notify characteristic shape and forwards
 * remote writes into @ref AnimationIsActiveBinding for the selected animation.
 *
 * Uses the fixed kIsActiveCharacteristicUuid (reused identically across every animation's
 * BtGattServer, like kAnimationNameCharacteristicUuid) rather than an auto-assigned UUID, so the
 * app can find "Is Active" the same way in every animation service without depending on each
 * animation's declaration order. See kIsActiveCharacteristicUuid's doc comment in
 * bt_service_cpp.h for the rationale.
 */
template <Animation tAnimationId>
class IsActiveCharacteristic
    : public BtGattCharacteristicCommon<IsActiveCharacteristic<tAnimationId>, "Is Active", true,
                                        false, bool, false> {
   public:
    using Base = BtGattCharacteristicCommon<IsActiveCharacteristic<tAnimationId>, "Is Active",
                                            true, false, bool, false>;
    using Base::operator=;

    IsActiveCharacteristic() { this->characteristic_uuid_ = kIsActiveCharacteristicUuid; }

    /**
     * @brief Updates local characteristic state and emits notify when changed.
     *
     * @param active New local active state.
     */
    void setActive(bool active) { this->operator=(active); }

    /**
     * @brief Reacts to remote writes by requesting animation activation.
     *
     * @param active Value written by the remote BLE client.
     */
    void onWrite(const bool &active) {
        AnimationIsActiveBinding<tAnimationId>::onRemoteActiveChange(active);
    }
};
