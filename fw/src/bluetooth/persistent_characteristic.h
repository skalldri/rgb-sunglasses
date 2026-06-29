#pragma once

#include <bluetooth/bt_gatt_traits.h>
#include <bluetooth/bt_service_cpp.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/sys/util_macro.h>

#include <algorithm>
#include <cstring>

/**
 * @brief BT-settable characteristic that persists its value via the Settings subsystem.
 *
 * CRTP wrapper over BtGattAutoCharacteristicExt, modeled directly on
 * IsActiveCharacteristic (see animation_is_active_characteristic.h): registers itself
 * with persistent_value_registry at construction, restores its value from settings (if
 * any was loaded) before BT comes up, and debounces a flash save after every remote
 * write rather than writing on every keystroke/drag.
 *
 * Only handles plain POD / BtGattColor / BtGattString<N> storage - BtGattDropdownList<N>
 * characteristics (e.g. glim selection/loop mode) already have bespoke write semantics
 * and persist by hand instead of through this mixin.
 */
template <StringLiteral Key, StringLiteral Description, bool Notify, typename T, T Default>
class BtGattPersistentCharacteristic
    : public BtGattAutoCharacteristicExt<
          BtGattPersistentCharacteristic<Key, Description, Notify, T, Default>, Description, Notify,
          false /* ReadOnly: persisted values are always read/write */, T, Default> {
   public:
    using Base = BtGattAutoCharacteristicExt<
        BtGattPersistentCharacteristic<Key, Description, Notify, T, Default>, Description, Notify,
        false, T, Default>;
    using Base::operator=;

    BtGattPersistentCharacteristic() {
        // Discarded entirely (no doLoad/doSave codegen, no registry call) when
        // CONFIG_APP_PERSIST_BT_CONFIG=n, e.g. on rgb_sunglasses_dk - see fw/Kconfig.
        // Failures (duplicate/overflow) are logged inside persistent_value_registry_register()
        // itself, which already has the key for context - no need to duplicate that here.
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_register(Key.value, this, &doLoad, &doSave);
        }
    }

    // Invoked by a remote BLE write (never by the operator= restore in doLoad, which
    // bypasses onWrite entirely - see BtGattWriteHook in bt_service_cpp.h).
    void onWrite(const T &) {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(Key.value);
            persistent_value_store::request_save();
        }
    }

    // Marks this characteristic dirty for the next batch save. Call before request_save()
    // when the value changes via a non-BLE path (e.g. a shell setter using operator=).
    void mark_dirty() {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(Key.value);
        }
    }

   private:
    static void doLoad(void *target, const void *data, size_t len) {
        auto *self = static_cast<BtGattPersistentCharacteristic *>(target);

        if constexpr (BtGattStringTraits<T>::kIsString) {
            constexpr size_t kMaxLen = BtGattStringTraits<T>::kMaxLen;
            T loaded{};
            size_t copyLen = std::min(len, kMaxLen - 1);
            memcpy(loaded.data(), data, copyLen);
            loaded[copyLen] = '\0';
            *self = loaded;
        } else {
            if (len != sizeof(T)) {
                return;
            }
            T loaded;
            memcpy(&loaded, data, sizeof(T));
            *self = loaded;
        }
    }

    static void doSave(void *target) {
        auto *self = static_cast<BtGattPersistentCharacteristic *>(target);
        T current = self->value();

        if constexpr (BtGattStringTraits<T>::kIsString) {
            constexpr size_t kMaxLen = BtGattStringTraits<T>::kMaxLen;
            size_t len = strnlen(current.data(), kMaxLen);
            persistent_value_store::save_value(Key.value, current.data(), len + 1);
        } else {
            persistent_value_store::save_value(Key.value, &current, sizeof(current));
        }
    }
};
