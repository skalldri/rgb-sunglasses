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

#include <battery_soc.h>
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

/* ---- Recording fakes for the calls battery_service.cpp makes ----
 * The BLE toggle now routes through the charger policy (the single owner of
 * BQ config writes), so the recording fake stands in for
 * charger_policy_set_user_charge_enable rather than the raw driver setter. */

static int fake_set_charge_enable_result;
static int fake_set_charge_enable_calls;
static bool fake_last_charge_enable;

extern "C" int charger_policy_set_user_charge_enable(bool enabled) {
    fake_set_charge_enable_calls++;
    fake_last_charge_enable = enabled;
    return fake_set_charge_enable_result;
}

static int fake_set_charge_current_result;
static int fake_set_charge_current_calls;
static uint32_t fake_last_charge_current;

extern "C" int charger_policy_set_charge_current_ma(uint32_t ma) {
    fake_set_charge_current_calls++;
    fake_last_charge_current = ma;
    return fake_set_charge_current_result;
}

/* ---- Helpers ---- */

/* Locates the Nth writable 128-bit-UUID value attribute in the battery
 * service, in declaration order (the CCC attrs are 16-bit UUIDs). Index 0 =
 * "Charging Enabled" (position 5), index 1 = "Charge Current (mA)"
 * (position 6) — writable characteristics appear in the same order as the
 * BtGattServer declaration. */
static const struct bt_gatt_attr *find_writable_attr(size_t index) {
    size_t seen = 0;

    STRUCT_SECTION_FOREACH(bt_gatt_service_static, svc) {
        for (size_t i = 0; i < svc->attr_count; i++) {
            const struct bt_gatt_attr *attr = &svc->attrs[i];
            if (attr->write != NULL && attr->uuid != NULL &&
                attr->uuid->type == BT_UUID_TYPE_128) {
                if (seen == index) {
                    return attr;
                }
                seen++;
            }
        }
    }

    return NULL;
}

/* Locates the Nth 128-bit-UUID value attribute (readable, declaration order).
 * The chrc/CUD/CPF/CCC descriptor attributes all carry 16-bit UUIDs, so the
 * 128-bit ones are exactly the characteristic value slots: index 0 = "Battery
 * Voltage (mV)" (position 0) ... index 7 = "Battery Percent" (position 7). */
static const struct bt_gatt_attr *find_value_attr(size_t index) {
    size_t seen = 0;

    STRUCT_SECTION_FOREACH(bt_gatt_service_static, svc) {
        for (size_t i = 0; i < svc->attr_count; i++) {
            const struct bt_gatt_attr *attr = &svc->attrs[i];
            if (attr->read != NULL && attr->uuid != NULL &&
                attr->uuid->type == BT_UUID_TYPE_128) {
                if (seen == index) {
                    return attr;
                }
                seen++;
            }
        }
    }

    return NULL;
}

static ssize_t write_charge_enable(bool enabled) {
    const struct bt_gatt_attr *attr = find_writable_attr(0);
    zassert_not_null(attr, "Charging Enabled value attribute not found");

    uint8_t value = enabled ? 1 : 0;
    return attr->write(NULL, attr, &value, sizeof(value), 0 /* offset */, 0 /* flags */);
}

static ssize_t write_charge_current(uint32_t ma) {
    const struct bt_gatt_attr *attr = find_writable_attr(1);
    zassert_not_null(attr, "Charge Current value attribute not found");

    return attr->write(NULL, attr, &ma, sizeof(ma), 0 /* offset */, 0 /* flags */);
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
    fake_set_charge_current_result = 0;
    fake_set_charge_current_calls = 0;
}

ZTEST_SUITE(battery_service_tests, NULL, NULL, reset_fakes, NULL, NULL);

ZTEST(battery_service_tests, test_charge_enable_registered_for_persistence) {
    /* Both persisted characteristics self-register their settings keys. */
    zassert_equal(persistent_value_registry_count(), 2);
}

ZTEST(battery_service_tests, test_charge_current_in_range_applies_and_persists) {
    ssize_t ret = write_charge_current(1200);

    zassert_equal(ret, 4, "successful write must return the payload length, got %zd", ret);
    zassert_equal(fake_set_charge_current_calls, 1);
    zassert_equal(fake_last_charge_current, 1200);
    zassert_equal(battery_service_get_charge_current_ma(), 1200);
}

ZTEST(battery_service_tests, test_charge_current_out_of_range_rejected) {
    uint32_t before = battery_service_get_charge_current_ma();

    /* Above CONFIG_APP_CHARGE_CURRENT_MAX_MA (2000): rejected, never applied. */
    ssize_t ret = write_charge_current(3000);
    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED));
    zassert_equal(fake_set_charge_current_calls, 0, "out-of-range must not reach the policy");
    zassert_equal(battery_service_get_charge_current_ma(), before, "storage must roll back");

    /* Below the 50mA ICHG floor: same. */
    ret = write_charge_current(10);
    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED));
    zassert_equal(fake_set_charge_current_calls, 0);

    /* Not a 10mA multiple (ICHG LSB): rejected so the app's stored value can
     * never disagree with the quantized value the policy would program. */
    ret = write_charge_current(905);
    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED));
    zassert_equal(fake_set_charge_current_calls, 0);
}

ZTEST(battery_service_tests, test_charge_current_policy_failure_rejects_and_rolls_back) {
    uint32_t before = battery_service_get_charge_current_ma();

    fake_set_charge_current_result = -EIO;
    ssize_t ret = write_charge_current(1000);

    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED));
    zassert_equal(fake_set_charge_current_calls, 1, "the policy write must have been attempted");
    zassert_equal(battery_service_get_charge_current_ma(), before, "storage must roll back");
}

/* The boot-time EN_CHG apply moved into charger_policy_boot_init() (which has
 * its own emulator-backed suite, fw/tests/power/charger_policy) — this service
 * only exposes the persisted intent for the charger thread to feed it. */
ZTEST(battery_service_tests, test_get_charge_enable_reflects_persisted_value) {
    load_persisted_charge_enable(false);
    zassert_false(battery_service_get_charge_enable());

    load_persisted_charge_enable(true);
    zassert_true(battery_service_get_charge_enable());

    /* Reading the intent must not generate charger traffic. */
    zassert_equal(fake_set_charge_enable_calls, 0);
}

ZTEST(battery_service_tests, test_remote_write_applies_to_charger) {
    load_persisted_charge_enable(true);

    ssize_t ret = write_charge_enable(false);

    zassert_equal(ret, 1, "successful write must return the payload length, got %zd", ret);
    zassert_equal(fake_set_charge_enable_calls, 1);
    zassert_false(fake_last_charge_enable);

    /* The characteristic's stored value follows the write. */
    battery_service_update(7910, -350, 0, 0, 0); /* unrelated telemetry doesn't disturb it */
    zassert_false(battery_service_get_charge_enable());
}

ZTEST(battery_service_tests, test_failed_charger_write_rejects_and_rolls_back) {
    load_persisted_charge_enable(true);

    fake_set_charge_enable_result = -EIO;
    ssize_t ret = write_charge_enable(false);

    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED),
                  "an I2C failure must reject the ATT write, got %zd", ret);
    zassert_equal(fake_set_charge_enable_calls, 1, "the charger write must have been attempted");

    /* Storage must have rolled back to the previous value (true). */
    zassert_true(battery_service_get_charge_enable(),
                 "storage must roll back after a rejected write");
}

ZTEST(battery_service_tests, test_battery_percent_publishes_from_vbat) {
    /* Position 7 = "Battery Percent", derived inside battery_service_update()
     * from vbat_mv via the shared battery_soc.h curve. */
    const struct bt_gatt_attr *attr = find_value_attr(7);
    zassert_not_null(attr, "Battery Percent value attribute not found");

    battery_service_update(7910, -350, 5000, 1000, 3);

    uint8_t percent = 0xFF;
    ssize_t ret = attr->read(NULL, attr, &percent, sizeof(percent), 0);
    zassert_equal(ret, 1, "percent read must return 1 byte, got %zd", ret);
    zassert_equal(percent, battery_soc_percent(7910),
                  "published percent must match the shared curve");
    zassert_equal(percent, 70, "7910 mV is 70%% on the 2S curve, got %u", percent);

    /* Full pack publishes 100; a dead/absent pack publishes 0. */
    battery_service_update(8400, 0, 5000, 0, 7);
    zassert_equal(attr->read(NULL, attr, &percent, sizeof(percent), 0), 1);
    zassert_equal(percent, 100);

    battery_service_update(6200, -350, 0, 0, 0);
    zassert_equal(attr->read(NULL, attr, &percent, sizeof(percent), 0), 1);
    zassert_equal(percent, 0);
}

ZTEST(battery_service_tests, test_update_publishes_without_charger_traffic) {
    /* Telemetry publication touches only the GATT characteristics — never the BQ. */
    battery_service_update(7914, -346, 5003, 998, 3);
    battery_service_update(7914, -346, 5003, 998, 3); /* unchanged → no-notify path */
    battery_service_update(8400, 500, 5100, 1500, 7); /* changed values */

    zassert_equal(fake_set_charge_enable_calls, 0);
}
