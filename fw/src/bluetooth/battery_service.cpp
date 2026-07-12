#include "battery_service.h"

#include <battery_soc.h>
#include <battery_util.h>
#include <bluetooth/bt_service_cpp.h>
#include <power/charger_policy.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util_macro.h>

#include <cstring>

LOG_MODULE_REGISTER(battery_service, LOG_LEVEL_INF);

/* Service id 5 — ids 1-4 are taken by CoreConfig, AudioConfig, mcuboot_info and
 * mcuboot_updater. Characteristic UUIDs are auto-assigned in declaration order
 * (suffixes ...0000 through ...0007); the companion app's constants in
 * app/constants/bluetooth.ts must match that order. */
constexpr bt_uuid_128 kBatteryServiceUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 5, 0x56789abc0000));

/* Settings key for the charging toggle. Explicit stable literal — never derive
 * from declaration order (see BtGattPersistentCharacteristic's Key doc). */
static constexpr const char kChargeEnableKey[] = "battery/charge_enable";

/**
 * @brief "Charging Enabled" toggle: persisted, and applied to the BQ25792 on write.
 *
 * Mirrors BtGattPersistentCharacteristic's persistence (that mixin can't be
 * subclassed — its CRTP Self is itself, so a subclass's hooks would never
 * dispatch), but uses the fallible onWriteChecked hook instead of onWrite: if
 * the EN_CHG I2C write fails, the remote write is rejected with an ATT error
 * and storage rolls back, so the app's optimistic UI update reverts
 * deterministically (fw/CLAUDE.md: never "success + corrective notify").
 *
 * Defaults to ON per issue #97; the persisted value (if any) overrides it via
 * doLoad's operator= before BT comes up, which bypasses onWriteChecked — the
 * I2C side only happens later, when the charger status thread feeds
 * battery_service_get_charge_enable() into charger_policy_boot_init().
 */
class ChargeEnableCharacteristic
    : public BtGattAutoCharacteristicExt<ChargeEnableCharacteristic, "Charging Enabled",
                                         true /* Notify */, false /* ReadOnly */, bool,
                                         true /* Default: charging ON */> {
   public:
    using Base = BtGattAutoCharacteristicExt<ChargeEnableCharacteristic, "Charging Enabled", true,
                                             false, bool, true>;
    using Base::operator=;

    ChargeEnableCharacteristic() {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_register(&mPersistEntry, kChargeEnableKey, this, &doLoad,
                                               &doSave);
        }
    }

    // Invoked by a remote BLE write, after the value landed in storage. A non-zero
    // return makes the framework restore the previous value and fail the ATT write.
    // Routed through the charger policy (the single owner of BQ config writes): it
    // gates the actual EN_CHG hardware write on battery presence — with no battery
    // the intent is accepted + persisted but not energized (charger_policy.h). The
    // I2C transfer runs on the BT RX thread; the policy's own mutex serializes it
    // against the charger status thread.
    int onWriteChecked(const bool &enabled) {
        int ret = charger_policy_set_user_charge_enable(enabled);
        if (ret != 0) {
            LOG_ERR("EN_CHG write failed (%d); rejecting BLE write", ret);
            return ret;
        }

        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(kChargeEnableKey);
            persistent_value_store::request_save();
        }
        return 0;
    }

   private:
    // Caller-owned registry storage (see persistent_value_registry.h). Not #if-gated: this
    // is a concrete (non-template) class, so the CONFIG_APP_PERSIST_BT_CONFIG=n build still
    // semantically checks the disabled register() branch, which references it. It's a few
    // bytes of BSS and zero flash when persistence is off - DK's constraint is flash.
    PersistentValueRegistryEntry mPersistEntry{};

    // POD-only copies of BtGattPersistentCharacteristic's doLoad/doSave (bool storage).
    static void doLoad(void *target, const void *data, size_t len) {
        auto *self = static_cast<ChargeEnableCharacteristic *>(target);
        if (len != sizeof(bool)) {
            return;
        }
        bool loaded;
        memcpy(&loaded, data, sizeof(loaded));
        *self = loaded;
    }

    static void doSave(void *target) {
        auto *self = static_cast<ChargeEnableCharacteristic *>(target);
        bool current = self->value();
        persistent_value_store::save_value(kChargeEnableKey, &current, sizeof(current));
    }
};

/* Settings key for the charge-current setting. Explicit stable literal — never
 * derive from declaration order. */
static constexpr const char kChargeCurrentKey[] = "battery/charge_current_ma";

/**
 * @brief "Charge Current (mA)": persisted fast-charge current target (ICHG),
 * applied through the charger policy on write. Same shape as
 * ChargeEnableCharacteristic above: onWriteChecked rejects with an ATT error
 * (out-of-range request, or the bridged I2C write failed) and storage rolls
 * back, so the app's optimistic update reverts deterministically.
 */
class ChargeCurrentCharacteristic
    : public BtGattAutoCharacteristicExt<ChargeCurrentCharacteristic, "Charge Current (mA)",
                                         true /* Notify */, false /* ReadOnly */, uint32_t,
                                         CONFIG_APP_CHARGE_CURRENT_MA> {
   public:
    using Base = BtGattAutoCharacteristicExt<ChargeCurrentCharacteristic, "Charge Current (mA)",
                                             true, false, uint32_t, CONFIG_APP_CHARGE_CURRENT_MA>;
    using Base::operator=;

    ChargeCurrentCharacteristic() {
        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_register(&mPersistEntry, kChargeCurrentKey, this, &doLoad,
                                               &doSave);
        }
    }

    int onWriteChecked(const uint32_t &ma) {
        /* 50mA is the BQ25792 ICHG floor (SLUSDG1C Table 9-16); the ceiling is
         * the build's pack/wiring limit. Rejecting (rather than clamping)
         * keeps the app UI honest about what was actually programmed. */
        if (ma < 50 || ma > CONFIG_APP_CHARGE_CURRENT_MAX_MA) {
            LOG_ERR("charge current %u mA outside [50, %u]; rejecting", ma,
                    CONFIG_APP_CHARGE_CURRENT_MAX_MA);
            return -EINVAL;
        }

        int ret = charger_policy_set_charge_current_ma(ma);
        if (ret != 0) {
            LOG_ERR("ICHG write failed (%d); rejecting BLE write", ret);
            return ret;
        }

        if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_mark_dirty(kChargeCurrentKey);
            persistent_value_store::request_save();
        }
        return 0;
    }

   private:
    PersistentValueRegistryEntry mPersistEntry{};

    static void doLoad(void *target, const void *data, size_t len) {
        auto *self = static_cast<ChargeCurrentCharacteristic *>(target);
        if (len != sizeof(uint32_t)) {
            return;
        }
        uint32_t loaded;
        memcpy(&loaded, data, sizeof(loaded));
        *self = loaded;
    }

    static void doSave(void *target) {
        auto *self = static_cast<ChargeCurrentCharacteristic *>(target);
        uint32_t current = self->value();
        persistent_value_store::save_value(kChargeCurrentKey, &current, sizeof(current));
    }
};

BtGattPrimaryService<kBatteryServiceUuid> batteryPrimaryService;
BtGattAutoReadNotifyCharacteristic<"Battery Voltage (mV)", int32_t, 0> batteryVoltageMv;
BtGattAutoReadNotifyCharacteristic<"Battery Current (mA)", int32_t, 0> batteryCurrentMa;
BtGattAutoReadNotifyCharacteristic<"VBUS Voltage (mV)", int32_t, 0> vbusVoltageMv;
BtGattAutoReadNotifyCharacteristic<"VBUS Current (mA)", int32_t, 0> vbusCurrentMa;
BtGattAutoReadNotifyCharacteristic<"Charge Status", uint8_t, 0> chargeStatus;
ChargeEnableCharacteristic chargingEnabled;
/* Position 6 — APPEND-ONLY: UUIDs are positional (suffix ...0006); the app's
 * constants must match. */
ChargeCurrentCharacteristic chargeCurrentMa;
/* Position 7 (suffix ...0007) — estimated state of charge from the rest-voltage
 * curve in battery_soc.h (see its accuracy caveat). Derived in
 * battery_service_update() so power.cpp keeps feeding raw telemetry only. */
BtGattAutoReadNotifyCharacteristic<"Battery Percent", uint8_t, 0> batteryPercent;

BtGattServer batteryServer(batteryPrimaryService, batteryVoltageMv, batteryCurrentMa, vbusVoltageMv,
                           vbusCurrentMa, chargeStatus, chargingEnabled, chargeCurrentMa,
                           batteryPercent);
BT_GATT_SERVER_REGISTER(batteryServerStatic, batteryServer);

bool battery_service_get_charge_enable(void) { return chargingEnabled.value(); }

uint32_t battery_service_get_charge_current_ma(void) { return chargeCurrentMa.value(); }

void battery_service_update(int32_t vbat_mv, int32_t ibat_ma, int32_t vbus_mv, int32_t ibus_ma,
                            uint8_t chg_stat) {
    /* 10 mV / 10 mA steps — see battery_quantize() for why raw readings would
     * spam notifications. Assignment only notifies when the quantized value
     * actually changed. */
    batteryVoltageMv = battery_quantize(vbat_mv, 10);
    batteryCurrentMa = battery_quantize(ibat_ma, 10);
    vbusVoltageMv    = battery_quantize(vbus_mv, 10);
    vbusCurrentMa    = battery_quantize(ibus_ma, 10);
    chargeStatus     = chg_stat;
    /* Derived from the quantized voltage so a ±few-mV ADC jitter sitting on a
     * curve-segment boundary can't flip the percent (and notify) every tick. */
    batteryPercent   = battery_soc_percent(battery_quantize(vbat_mv, 10));
}
