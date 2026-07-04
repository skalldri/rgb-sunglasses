/*
 * Tests for the battery monitoring GATT service (fw/src/bluetooth/battery_service.cpp,
 * issue #97), compiled with the real BT host headers on native_sim.
 *
 * The BQ25792 driver functions battery_service.cpp calls are replaced with
 * recording fakes below (no I2C bus involved); the bq25792 devicetree node is a
 * placeholder from boards/native_sim.overlay with a no-op device defined here.
 * The "Charging Enabled" write path is driven through the service's actual GATT
 * attribute table (found via the bt_gatt_service_static iterable section), the
 * same entry point the ATT layer uses.
 */

#include <bluetooth/battery_service.h>
#include <settings/persistent_value_registry.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/ztest.h>

#include <cstring>

/* Device object for the placeholder bq25792 devicetree node. */
DEVICE_DT_DEFINE(DT_NODELABEL(bq25792), NULL, NULL, NULL, NULL, POST_KERNEL, 99, NULL);

/* ---- Recording fakes for the driver calls battery_service.cpp makes ---- */

static int fake_set_charge_enable_result;
static int fake_set_charge_enable_calls;
static bool fake_last_charge_enable;

static int fake_ibat_sense_result;
static int fake_ibat_sense_calls;
static bool fake_last_ibat_sense;

extern "C" int bq25792_set_charge_enable(const struct device *dev, bool enabled) {
    zassert_not_null(dev);
    fake_set_charge_enable_calls++;
    fake_last_charge_enable = enabled;
    return fake_set_charge_enable_result;
}

extern "C" int bq25792_ibat_sense_enable(const struct device *dev, bool enable) {
    zassert_not_null(dev);
    fake_ibat_sense_calls++;
    fake_last_ibat_sense = enable;
    return fake_ibat_sense_result;
}

/* ---- Helpers ---- */

/* Locates the writable 128-bit-UUID value attribute in the battery service —
 * only the "Charging Enabled" characteristic accepts writes (the CCC attrs are
 * 16-bit UUIDs), so this uniquely identifies it. */
static const struct bt_gatt_attr *find_charge_enable_attr(void) {
    const struct bt_gatt_attr *found = NULL;

    STRUCT_SECTION_FOREACH(bt_gatt_service_static, svc) {
        for (size_t i = 0; i < svc->attr_count; i++) {
            const struct bt_gatt_attr *attr = &svc->attrs[i];
            if (attr->write != NULL && attr->uuid != NULL &&
                attr->uuid->type == BT_UUID_TYPE_128) {
                zassert_is_null(found, "expected exactly one writable value attribute");
                found = attr;
            }
        }
    }

    return found;
}

static ssize_t write_charge_enable(bool enabled) {
    const struct bt_gatt_attr *attr = find_charge_enable_attr();
    zassert_not_null(attr, "Charging Enabled value attribute not found");

    uint8_t value = enabled ? 1 : 0;
    return attr->write(NULL, attr, &value, sizeof(value), 0 /* offset */, 0 /* flags */);
}

/* settings_read_cb feeding a persisted bool into the registry's load dispatch,
 * the same way settings_load() would after a reboot. */
static ssize_t read_persisted_bool(void *cb_arg, void *data, size_t len) {
    zassert_true(len >= sizeof(bool));
    memcpy(data, cb_arg, sizeof(bool));
    return sizeof(bool);
}

static void load_persisted_charge_enable(bool value) {
    int ret = persistent_value_registry_dispatch_load("battery/charge_enable", sizeof(bool),
                                                      read_persisted_bool, &value);
    zassert_equal(ret, 0, "dispatch_load failed: %d", ret);
}

static void reset_fakes(void *) {
    fake_set_charge_enable_result = 0;
    fake_set_charge_enable_calls = 0;
    fake_ibat_sense_result = 0;
    fake_ibat_sense_calls = 0;
}

ZTEST_SUITE(battery_service_tests, NULL, NULL, reset_fakes, NULL, NULL);

ZTEST(battery_service_tests, test_charge_enable_registered_for_persistence) {
    /* The ChargeEnableCharacteristic constructor self-registers its settings key. */
    zassert_equal(persistent_value_registry_count(), 1);
}

ZTEST(battery_service_tests, test_boot_state_applies_persisted_off) {
    load_persisted_charge_enable(false);

    battery_service_apply_boot_state();

    zassert_equal(fake_ibat_sense_calls, 1);
    zassert_true(fake_last_ibat_sense, "IBAT sensing must be enabled at boot");
    zassert_equal(fake_set_charge_enable_calls, 1);
    zassert_false(fake_last_charge_enable, "persisted OFF must be pushed to the charger");
}

ZTEST(battery_service_tests, test_boot_state_applies_persisted_on) {
    load_persisted_charge_enable(true);

    battery_service_apply_boot_state();

    zassert_equal(fake_set_charge_enable_calls, 1);
    zassert_true(fake_last_charge_enable);
}

ZTEST(battery_service_tests, test_remote_write_applies_to_charger) {
    load_persisted_charge_enable(true);

    ssize_t ret = write_charge_enable(false);

    zassert_equal(ret, 1, "successful write must return the payload length, got %zd", ret);
    zassert_equal(fake_set_charge_enable_calls, 1);
    zassert_false(fake_last_charge_enable);

    /* The characteristic's stored value follows the write: boot-apply now sends false. */
    battery_service_update(7910, -350, 0, 0, 0); /* unrelated telemetry doesn't disturb it */
    battery_service_apply_boot_state();
    zassert_false(fake_last_charge_enable);
}

ZTEST(battery_service_tests, test_failed_charger_write_rejects_and_rolls_back) {
    load_persisted_charge_enable(true);

    fake_set_charge_enable_result = -EIO;
    ssize_t ret = write_charge_enable(false);

    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED),
                  "an I2C failure must reject the ATT write, got %zd", ret);
    zassert_equal(fake_set_charge_enable_calls, 1, "the charger write must have been attempted");

    /* Storage must have rolled back to the previous value (true): a subsequent
     * boot-apply pushes true, not the rejected false. */
    fake_set_charge_enable_result = 0;
    battery_service_apply_boot_state();
    zassert_true(fake_last_charge_enable, "storage must roll back after a rejected write");
}

ZTEST(battery_service_tests, test_update_publishes_without_charger_traffic) {
    /* Telemetry publication touches only the GATT characteristics — never the BQ. */
    battery_service_update(7914, -346, 5003, 998, 3);
    battery_service_update(7914, -346, 5003, 998, 3); /* unchanged → no-notify path */
    battery_service_update(8400, 500, 5100, 1500, 7); /* changed values */

    zassert_equal(fake_set_charge_enable_calls, 0);
    zassert_equal(fake_ibat_sense_calls, 0);
}
