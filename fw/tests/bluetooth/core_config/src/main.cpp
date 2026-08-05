/*
 * Tests for the Core Config GATT service (fw/src/core_config.cpp), compiled with the
 * real BT host headers on native_sim — the same CONFIG_BT=y / no-bt_enable() pattern
 * as fw/tests/bluetooth/shuffle_service.
 *
 * Pins the Android-notification-budget contract added with the SMP-timeout fix:
 * exactly one notifiable characteristic in this service — the appended (position 4)
 * read-only "Active Animation" — with the four config values non-notifiable, plus the
 * ActiveAnimationBinding round trip that proves core_config.cpp's static registrar
 * actually ran (the PR #89 silent-registration failure class).
 */

#include <animations/active_animation_binding.h>
#include <animations/animation_types.h>
#include <core_config.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/ztest.h>

#include <cstring>

/* Plain `static` file-scope helpers, NOT an anonymous namespace — see the
 * animation_adapters suite's comment on STRUCT_SECTION_FOREACH's linker symbols. */

/* Locates the Nth 128-bit-UUID value attribute in declaration order (chrc/CUD/CPF/CCC
 * descriptor attrs all carry 16-bit UUIDs, as does everything in Zephyr's own GAP/GATT
 * static services, so the walk lands only in the Core Config table — the sole app
 * service this binary registers): 0 = Brightness, 1 = Display Thread Rate, 2 = Render
 * Thread Rate, 3 = Status LED Brightness, 4 = Active Animation. Same helper shape as
 * the shuffle_service suite. */
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

static size_t count_value_attrs(void) {
    size_t seen = 0;
    while (find_value_attr(seen) != NULL) {
        seen++;
    }
    return seen;
}

/* The characteristic-declaration (CHRC) attr sits immediately before its value attr;
 * returns it so callers can inspect the bt_gatt_chrc properties (e.g. NOTIFY). */
static const struct bt_gatt_attr *chrc_decl_for(const struct bt_gatt_attr *valueAttr) {
    STRUCT_SECTION_FOREACH(bt_gatt_service_static, svc) {
        for (size_t i = 1; i < svc->attr_count; i++) {
            if (&svc->attrs[i] == valueAttr) {
                const struct bt_gatt_attr *prev = &svc->attrs[i - 1];
                return (prev->uuid && bt_uuid_cmp(prev->uuid, BT_UUID_GATT_CHRC) == 0) ? prev
                                                                                       : NULL;
            }
        }
    }
    return NULL;
}

static uint8_t chrc_properties(const struct bt_gatt_attr *valueAttr) {
    const struct bt_gatt_attr *chrc = chrc_decl_for(valueAttr);
    zassert_not_null(chrc, "value attr has no CHRC declaration");
    return static_cast<const struct bt_gatt_chrc *>(chrc->user_data)->properties;
}

static uint32_t read_u32(const struct bt_gatt_attr *attr) {
    uint32_t value = 0;
    ssize_t len = attr->read(NULL, attr, &value, sizeof(value), 0);
    zassert_equal(len, sizeof(value), "expected a 4-byte read, got %zd", len);
    return value;
}

/* Snapshot of the constructed default, taken once before any case runs (ztest runs
 * cases in name order; the suite-setup snapshot makes the defaults assertion
 * order-independent — same rationale as the shuffle_service suite). */
static uint32_t sDefaultActiveAnimation;

static void *snapshot_defaults(void) {
    const struct bt_gatt_attr *attr = find_value_attr(4);
    sDefaultActiveAnimation = (attr != NULL) ? read_u32(attr) : 0xdead;
    return NULL;
}

ZTEST_SUITE(core_config_service, NULL, snapshot_defaults, NULL, NULL, NULL);

ZTEST(core_config_service, test_layout_and_notify_budget) {
    /* Layout pin (append-only rule): exactly 5 value attributes, positions 0-3 the
     * pre-existing config values, position 4 the appended Active Animation. */
    zassert_equal(count_value_attrs(), 5, "Core Config must expose exactly 5 characteristics");

    /* Position 0 is Brightness with its documented default — guards against a
     * reorder, which would silently shift every positional auto-UUID. */
    zassert_equal(read_u32(find_value_attr(0)), 20, "position 0 must be Brightness (default 20)");

    /* The four config values must NOT be notifiable — each notify costs one of
     * Android's ~15 registration slots (BTA_GATTC_NOTIF_REG_MAX), the budget bug
     * that starved the SMP characteristic. They stay writable. */
    for (size_t i = 0; i < 4; i++) {
        uint8_t props = chrc_properties(find_value_attr(i));
        zassert_false(props & BT_GATT_CHRC_NOTIFY, "config characteristic %zu must not notify", i);
        zassert_true(props & BT_GATT_CHRC_WRITE, "config characteristic %zu must stay writable", i);
    }

    /* Active Animation is the service's single notifiable characteristic, and is
     * read-only: activation writes go through the per-animation Is Active
     * characteristics, never through this one. */
    const struct bt_gatt_attr *active = find_value_attr(4);
    zassert_not_null(active, "Active Animation value attribute not found");
    uint8_t props = chrc_properties(active);
    zassert_true(props & BT_GATT_CHRC_NOTIFY, "Active Animation must be notifiable");
    zassert_false(props & BT_GATT_CHRC_WRITE, "Active Animation must not be writable");
    zassert_false(props & BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                  "Active Animation must not be writable");
    zassert_is_null(active->write, "Active Animation must have no write handler");
    zassert_equal(active->perm & (BT_GATT_PERM_WRITE_AUTHEN | BT_GATT_PERM_PREPARE_WRITE), 0,
                  "Active Animation must carry no write permissions");
}

ZTEST(core_config_service, test_active_animation_binding_round_trip) {
    /* Default: 0 = Animation::None (the pre-boot-restore state and the deliberate
     * "off" state). Asserted from the suite-setup snapshot. */
    zassert_equal(sDefaultActiveAnimation, static_cast<uint32_t>(Animation::None),
                  "Active Animation must default to Animation::None");

    const struct bt_gatt_attr *active = find_value_attr(4);
    zassert_not_null(active);

    /* Round trip through the BT-free binding — the exact call
     * pattern_controller_change_to_animation() makes. Passing proves
     * core_config.cpp's static registrar actually registered the setter. */
    ActiveAnimationBinding::setLocalActiveAnimation(Animation::Rainbow);
    zassert_equal(read_u32(active), static_cast<uint32_t>(Animation::Rainbow));

    /* Extension ids live at 0x40 + slot (extension_limits.h) — outside the enum's
     * named values but inside the characteristic's uint32 domain. */
    ActiveAnimationBinding::setLocalActiveAnimation(static_cast<Animation>(0x40));
    zassert_equal(read_u32(active), 0x40u);

    ActiveAnimationBinding::setLocalActiveAnimation(Animation::None);
    zassert_equal(read_u32(active), static_cast<uint32_t>(Animation::None));
}
