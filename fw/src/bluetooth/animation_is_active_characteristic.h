#pragma once

#include <animations/animation_is_active_binding.h>
#include <bluetooth/bt_service_cpp.h>

/**
 * @brief Reusable BLE `Is Active` characteristic for a specific animation.
 *
 * Provides the common read/write characteristic shape and forwards
 * remote writes into @ref AnimationIsActiveBinding for the selected animation.
 *
 * Deliberately NOT notifiable: Android caps GATT notification registrations at
 * ~15 per app (BTA_GATTC_NOTIF_REG_MAX), and one notify per animation service
 * blew that budget and silently starved the SMP/DFU characteristic's
 * registration. Device-side activation changes reach the app through Core
 * Config's single "Active Animation" characteristic instead (core_config.cpp).
 *
 * Uses the fixed kIsActiveCharacteristicUuid (reused identically across every animation's
 * BtGattServer, like kAnimationNameCharacteristicUuid) rather than an auto-assigned UUID, so the
 * app can find "Is Active" the same way in every animation service without depending on each
 * animation's declaration order. See kIsActiveCharacteristicUuid's doc comment in
 * bt_service_cpp.h for the rationale.
 */
template <Animation tAnimationId>
class IsActiveCharacteristic
    : public BtGattCharacteristicCommon<IsActiveCharacteristic<tAnimationId>, "Is Active", false,
                                        false, bool, false> {
   public:
    using Base = BtGattCharacteristicCommon<IsActiveCharacteristic<tAnimationId>, "Is Active",
                                            false, false, bool, false>;
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
