#pragma once

#include <bluetooth/bt_service_cpp.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/sys/util_macro.h>

#include <cstring>

/**
 * @brief Reusable, persisted BLE "Include in Shuffle" characteristic for a specific
 * animation (issue #243).
 *
 * Uses the fixed kShuffleIncludeCharacteristicUuid (reused identically across every
 * animation's BtGattServer, like kIsActiveCharacteristicUuid) rather than an
 * auto-assigned UUID, so the app can find "Include in Shuffle" the same way in every
 * animation service without depending on each animation's declaration order. See
 * kShuffleIncludeCharacteristicUuid's doc comment in bt_service_cpp.h for the rationale.
 *
 * Persistence mirrors BtGattPersistentCharacteristic (that mixin can't be reused here:
 * it is auto-UUID and CRTP-closed — its Self is itself, so a subclass's fixed-UUID ctor
 * and hooks would never apply). @p Key is an explicit stable string literal (e.g.
 * "pulse/shuffle") — never derive it from declaration order.
 *
 * Defaults to true: every animation participates in shuffle until the user opts it out.
 * A plain onWrite (not onWriteChecked) is deliberate — storing a bool has no fallible
 * side effect, so there is no ATT-reject path; eligibility is *pulled* by the shuffle
 * pool at pick time (see AnimationShuffleIncludeBinding), never pushed on write.
 */
template <StringLiteral Key>
class ShuffleIncludeCharacteristic
    : public BtGattCharacteristicCommon<ShuffleIncludeCharacteristic<Key>, "Include in Shuffle",
                                        true /* Notify */, false /* ReadOnly */, bool,
                                        true /* Default: included */> {
   public:
    using Base = BtGattCharacteristicCommon<ShuffleIncludeCharacteristic<Key>, "Include in Shuffle",
                                            true, false, bool, true>;
    using Base::operator=;

    ShuffleIncludeCharacteristic() {
        this->characteristic_uuid_ = kShuffleIncludeCharacteristicUuid;
        // Failures (duplicate key) are logged inside persistent_value_registry_register()
        // itself, which already has the key for context.
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_register(&mPersistEntry, Key.value, this, &doLoad, &doSave);
        }
    }

    // Invoked by a remote BLE write (never by the operator= restore in doLoad, which
    // bypasses onWrite entirely - see BtGattWriteHook in bt_service_cpp.h).
    void onWrite(const bool &) {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(Key.value);
            persistent_value_store::request_save();
        }
    }

   private:
    // Caller-owned registry storage (see persistent_value_registry.h). Not #if-gated:
    // the register() call above is inside `if constexpr`, but an unqualified member name
    // must still resolve at template-definition time (same note as
    // BtGattPersistentCharacteristic's entry).
    PersistentValueRegistryEntry mPersistEntry{};

    // POD-only copies of BtGattPersistentCharacteristic's doLoad/doSave (bool storage).
    static void doLoad(void *target, const void *data, size_t len) {
        auto *self = static_cast<ShuffleIncludeCharacteristic *>(target);
        if (len != sizeof(bool)) {
            return;
        }
        bool loaded;
        memcpy(&loaded, data, sizeof(loaded));
        *self = loaded;
    }

    static void doSave(void *target) {
        auto *self = static_cast<ShuffleIncludeCharacteristic *>(target);
        bool current = self->value();
        persistent_value_store::save_value(Key.value, &current, sizeof(current));
    }
};
