#include "battery_service.h"

#include <battery_util.h>
#include <bluetooth/bt_service_cpp.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util_macro.h>

#include <cstring>

LOG_MODULE_REGISTER(battery_service, LOG_LEVEL_INF);

/* This file is only compiled when CONFIG_APP_BATTERY_MONITOR=y (never in ztest
 * builds), so unlike power.cpp it needs no CONFIG_ZTEST null-device fallback. */
static const struct device *bq = DEVICE_DT_GET(DT_NODELABEL(bq25792));

/* Service id 5 — ids 1-4 are taken by CoreConfig, AudioConfig, mcuboot_info and
 * mcuboot_updater. Characteristic UUIDs are auto-assigned in declaration order
 * (suffixes ...0000 through ...0005); the companion app's constants in
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
 * I2C side only happens later, from battery_service_apply_boot_state() on the
 * charger status thread.
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
            persistent_value_registry_register(kChargeEnableKey, this, &doLoad, &doSave);
        }
    }

    // Invoked by a remote BLE write, after the value landed in storage. A non-zero
    // return makes the framework restore the previous value and fail the ATT write.
    // The EN_CHG I2C transfer runs on the BT RX thread, concurrent with the charger
    // status thread's ADC reads — different registers, and Zephyr's I2C bus lock
    // serializes the transfers, so this is safe.
    int onWriteChecked(const bool &enabled) {
        int ret = bq25792_set_charge_enable(bq, enabled);
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

BtGattPrimaryService<kBatteryServiceUuid> batteryPrimaryService;
BtGattAutoReadNotifyCharacteristic<"Battery Voltage (mV)", int32_t, 0> batteryVoltageMv;
BtGattAutoReadNotifyCharacteristic<"Battery Current (mA)", int32_t, 0> batteryCurrentMa;
BtGattAutoReadNotifyCharacteristic<"VBUS Voltage (mV)", int32_t, 0> vbusVoltageMv;
BtGattAutoReadNotifyCharacteristic<"VBUS Current (mA)", int32_t, 0> vbusCurrentMa;
BtGattAutoReadNotifyCharacteristic<"Charge Status", uint8_t, 0> chargeStatus;
ChargeEnableCharacteristic chargingEnabled;

BtGattServer batteryServer(batteryPrimaryService, batteryVoltageMv, batteryCurrentMa, vbusVoltageMv,
                           vbusCurrentMa, chargeStatus, chargingEnabled);
BT_GATT_SERVER_REGISTER(batteryServerStatic, batteryServer);

void battery_service_apply_boot_state(void) {
    int ret = bq25792_ibat_sense_enable(bq, true);
    if (ret != 0) {
        LOG_ERR("Failed to enable IBAT sensing: %d", ret);
    }

    bool enabled = chargingEnabled.value();
    ret = bq25792_set_charge_enable(bq, enabled);
    if (ret != 0) {
        LOG_ERR("Failed to apply persisted EN_CHG=%u: %d", enabled ? 1 : 0, ret);
    } else {
        LOG_INF("Charging %s (persisted setting)", enabled ? "enabled" : "disabled");
    }
}

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
}
