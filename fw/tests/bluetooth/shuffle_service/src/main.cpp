/*
 * Tests for the dedicated Shuffle GATT service (fw/src/bluetooth/shuffle_service.cpp,
 * issue #243), compiled with the real BT host headers on native_sim — the same
 * CONFIG_BT=y / no-bt_enable() pattern as fw/tests/bluetooth/battery_service.
 *
 * Covers the three characteristics moved out of Core Config: defaults, the remote
 * GATT write path (driven through the service's actual static attribute table, the
 * same entry point the ATT layer uses), the shell setter, the deliberate
 * min-may-exceed-max tolerance, and that the "shuffle/*" settings keys are
 * registered and restore values through the registry's real load dispatch.
 */

#include <bluetooth/shuffle_service.h>
#include <settings/persistent_value_registry.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/ztest.h>

#include <cerrno>
#include <cstring>

/* Plain `static` file-scope helpers, NOT an anonymous namespace — see the
 * animation_adapters suite's comment on STRUCT_SECTION_FOREACH's linker symbols. */

/* Locates the Nth 128-bit-UUID value attribute in declaration order (the
 * chrc/CUD/CPF/CCC descriptor attrs all carry 16-bit UUIDs): index 0 =
 * "Shuffle Enabled", 1 = "Shuffle Min Duration (s)", 2 = "Shuffle Max
 * Duration (s)". This binary registers only the shuffle service, so the walk
 * cannot land in another service's table. */
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

static ssize_t do_write(const struct bt_gatt_attr *attr, const void *data, size_t len) {
    return attr->write(NULL, attr, data, len, 0 /* offset */, 0 /* flags */);
}

/* The CHRC declaration attr sits immediately before its value attr; used to assert the
 * notification-budget contract below. */
static bool chrc_has_notify(const struct bt_gatt_attr *valueAttr) {
    STRUCT_SECTION_FOREACH(bt_gatt_service_static, svc) {
        for (size_t i = 1; i < svc->attr_count; i++) {
            if (&svc->attrs[i] == valueAttr) {
                const struct bt_gatt_attr *prev = &svc->attrs[i - 1];
                if (prev->uuid == NULL || bt_uuid_cmp(prev->uuid, BT_UUID_GATT_CHRC) != 0) {
                    return false;
                }
                return (static_cast<const struct bt_gatt_chrc *>(prev->user_data)->properties &
                        BT_GATT_CHRC_NOTIFY) != 0;
            }
        }
    }
    return false;
}

/* settings_read_cb feeding a persisted value into the registry's load dispatch,
 * the same way settings_load() would after a reboot. */
static ssize_t read_persisted_bool(void *cb_arg, void *data, size_t len) {
    zassert_true(len >= sizeof(bool));
    memcpy(data, cb_arg, sizeof(bool));
    return sizeof(bool);
}

static ssize_t read_persisted_u32(void *cb_arg, void *data, size_t len) {
    zassert_true(len >= sizeof(uint32_t));
    memcpy(data, cb_arg, sizeof(uint32_t));
    return sizeof(uint32_t);
}

/* Snapshot of the constructed defaults, taken once before any case runs. */
static bool sDefaultEnabled;
static uint32_t sDefaultMinS;
static uint32_t sDefaultMaxS;

/* The three characteristics are process-global statics, and nothing can restore
 * their constructed defaults once a case has written them — so the pristine state
 * is a once-per-process observation, not something a per-test `before` hook could
 * re-establish (a hook would only be asserting the values it had just written).
 * Capturing it in the suite setup, which ztest runs once ahead of every case,
 * makes the defaults assertion below independent of execution order. Without this
 * it passed only because ztest runs cases in name order and "test_defaults..."
 * happens to sort ahead of "test_persisted..." — a future case sorting between
 * them would have broken it. */
static void *snapshot_defaults(void) {
    sDefaultEnabled = shuffle_service_get_enabled();
    sDefaultMinS = shuffle_service_get_min_duration_s();
    sDefaultMaxS = shuffle_service_get_max_duration_s();
    return NULL;
}

ZTEST_SUITE(shuffle_service, NULL, snapshot_defaults, NULL, NULL, NULL);

ZTEST(shuffle_service, test_defaults_writes_and_shell_setter) {
    /* Defaults: disabled, 30 s .. 120 s (carried over from the Core Config
     * originals, issue #121). Asserted against the suite-setup snapshot, so this
     * holds no matter which case ztest runs first. */
    zassert_false(sDefaultEnabled, "shuffle must default to off");
    zassert_equal(sDefaultMinS, 30);
    zassert_equal(sDefaultMaxS, 120);

    const struct bt_gatt_attr *enabledAttr = find_value_attr(0);
    const struct bt_gatt_attr *minAttr = find_value_attr(1);
    const struct bt_gatt_attr *maxAttr = find_value_attr(2);
    zassert_not_null(enabledAttr, "Shuffle Enabled value attribute not found");
    zassert_not_null(minAttr, "Shuffle Min Duration value attribute not found");
    zassert_not_null(maxAttr, "Shuffle Max Duration value attribute not found");

    /* Remote write path: enable, retime. */
    uint8_t one = 1;
    zassert_equal(do_write(enabledAttr, &one, sizeof(one)), sizeof(one));
    zassert_true(shuffle_service_get_enabled());

    uint32_t minS = 45;
    zassert_equal(do_write(minAttr, &minS, sizeof(minS)), sizeof(minS));
    zassert_equal(shuffle_service_get_min_duration_s(), 45);

    uint32_t maxS = 60;
    zassert_equal(do_write(maxAttr, &maxS, sizeof(maxS)), sizeof(maxS));
    zassert_equal(shuffle_service_get_max_duration_s(), 60);

    /* min > max is deliberately ACCEPTED (the two are written one at a time
     * over BLE; ShuffleController swaps at pick time — see shuffle_service.cpp). */
    uint32_t hugeMin = 500;
    zassert_equal(do_write(minAttr, &hugeMin, sizeof(hugeMin)), sizeof(hugeMin));
    zassert_equal(shuffle_service_get_min_duration_s(), 500);
    zassert_equal(shuffle_service_get_max_duration_s(), 60);

    /* Oversized write is rejected and leaves the value untouched. */
    uint8_t tooLong[8] = {};
    zassert_equal(do_write(minAttr, tooLong, sizeof(tooLong)),
                 BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    zassert_equal(shuffle_service_get_min_duration_s(), 500);

    /* Shell path (anim shuffle on|off): plain setter round trip. */
    shuffle_service_set_enabled(false);
    zassert_false(shuffle_service_get_enabled());
    shuffle_service_set_enabled(true);
    zassert_true(shuffle_service_get_enabled());

    /* Notification budget (Android's ~15-slot BTA_GATTC_NOTIF_REG_MAX): none of the three
     * notify. All are app-written; the only device-side writer is the developer-only
     * `anim shuffle` shell command above, which is not a user-visible scenario. */
    zassert_false(chrc_has_notify(enabledAttr), "Shuffle Enabled must NOT expose NOTIFY");
    zassert_false(chrc_has_notify(minAttr), "Shuffle Min Duration must NOT expose NOTIFY");
    zassert_false(chrc_has_notify(maxAttr), "Shuffle Max Duration must NOT expose NOTIFY");
}

ZTEST(shuffle_service, test_persisted_keys_registered_and_loaded) {
    /* The three "shuffle/*" keys must be registered with the persistence
     * registry — proven end to end by driving the registry's real load
     * dispatch (the settings_load() replay path) and observing the restored
     * values through the service getters. */
    bool persistedEnabled = true;
    zassert_equal(persistent_value_registry_dispatch_load("shuffle/enabled", sizeof(bool),
                                                          read_persisted_bool, &persistedEnabled),
                  0, "'shuffle/enabled' must be a registered key");
    zassert_true(shuffle_service_get_enabled());

    uint32_t persistedMin = 7;
    zassert_equal(persistent_value_registry_dispatch_load("shuffle/min_s", sizeof(uint32_t),
                                                          read_persisted_u32, &persistedMin),
                  0, "'shuffle/min_s' must be a registered key");
    zassert_equal(shuffle_service_get_min_duration_s(), 7);

    uint32_t persistedMax = 99;
    zassert_equal(persistent_value_registry_dispatch_load("shuffle/max_s", sizeof(uint32_t),
                                                          read_persisted_u32, &persistedMax),
                  0, "'shuffle/max_s' must be a registered key");
    zassert_equal(shuffle_service_get_max_duration_s(), 99);

    /* The pre-#243 Core Config keys are gone — their orphaned NVS entries
     * must fall through the dispatch (ignored), not land somewhere. */
    bool stale = false;
    zassert_equal(persistent_value_registry_dispatch_load("core/shuffle_enabled", sizeof(bool),
                                                          read_persisted_bool, &stale),
                  -ENOENT, "the old core/shuffle_enabled key must no longer be registered");
}
