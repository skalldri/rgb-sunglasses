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
 * Only handles plain POD / BtGattColor / BtGattString<N> storage (and their wire-compatible
 * slot wrappers BtGattSlotString<N> / BtGattSlotUpNext, which forward the same data()/compare
 * operations or convert to uint32 — same persisted byte layout, different CPF format) -
 * BtGattDropdownList<N> characteristics (e.g. glim selection/loop mode) already have bespoke
 * write semantics and persist by hand instead of through this mixin.
 *
 * @tparam OnRemoteWrite Optional side effect to run after a remote write has been stored
 * and marked dirty — for cross-characteristic invariants that would otherwise force a
 * bespoke CRTP copy of all the persistence boilerplate below (this class is CRTP-closed:
 * its Self is itself, so a subclass's own onWrite would never be dispatched — same
 * constraint ShuffleIncludeCharacteristic documents). Defaults to the empty
 * @ref bt_gatt_no_write_hook, which the compiler inlines away, so every existing
 * declaration costs exactly what it did before. (A `nullptr` default with a guarded call
 * would be the obvious spelling, but the guard compares a known-good function address
 * against null and GCC rejects that under -Werror=address.)
 * Runs on the BT RX thread. Assigning to another characteristic from here is safe and does
 * not recurse: operator= bypasses onWrite by design (see BtGattWriteHook in
 * bt_service_cpp.h). It does not notify or persist the sibling, so a hook that changes one
 * must call the sibling's own mark_dirty() and request a save.
 */
template <typename T>
inline void bt_gatt_no_write_hook(const T &) {}

template <StringLiteral Key, StringLiteral Description, bool Notify, typename T, T Default,
          void (*OnRemoteWrite)(const T &) = &bt_gatt_no_write_hook<T>>
class BtGattPersistentCharacteristic
    : public BtGattAutoCharacteristicExt<
          BtGattPersistentCharacteristic<Key, Description, Notify, T, Default, OnRemoteWrite>,
          Description, Notify,
          false /* ReadOnly: persisted values are always read/write */, T, Default> {
   public:
    using Base = BtGattAutoCharacteristicExt<
        BtGattPersistentCharacteristic<Key, Description, Notify, T, Default, OnRemoteWrite>,
        Description, Notify, false, T, Default>;
    using Base::operator=;

    BtGattPersistentCharacteristic() {
        // Discarded entirely (no doLoad/doSave codegen, no registry call) when
        // CONFIG_APP_PERSIST_BT_CONFIG=n, e.g. on the legacy DK board (dk-support
        // branch) - see fw/Kconfig.
        // Failures (duplicate key) are logged inside persistent_value_registry_register()
        // itself, which already has the key for context - no need to duplicate that here.
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_register(&mPersistEntry, Key.value, this, &doLoad, &doSave);
        }
    }

    // Invoked by a remote BLE write (never by the operator= restore in doLoad, which
    // bypasses onWrite entirely - see BtGattWriteHook in bt_service_cpp.h).
    void onWrite(const T &value) {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(Key.value);
            persistent_value_store::request_save();
        }
        OnRemoteWrite(value);
    }

    // Marks this characteristic dirty for the next batch save. Call before request_save()
    // when the value changes via a non-BLE path (e.g. a shell setter using operator=).
    void mark_dirty() {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(Key.value);
        }
    }

   private:
    // Registry storage is caller-owned (see persistent_value_registry.h); this instance is
    // a long-lived static, so it owns its own entry. Not #if-gated on
    // CONFIG_APP_PERSIST_BT_CONFIG: the register() call below is inside `if constexpr`, but
    // an unqualified member name must still resolve at template-definition time, so gating
    // the member out breaks the disabled build. It's a few bytes of BSS and zero flash when
    // persistence is off - the legacy DK board's constraint was flash, not RAM.
    PersistentValueRegistryEntry mPersistEntry{};

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
