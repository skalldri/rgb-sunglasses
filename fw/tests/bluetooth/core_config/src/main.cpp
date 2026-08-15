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

static ssize_t gatt_write_u32(const struct bt_gatt_attr *attr, uint32_t value) {
    zassert_not_null(attr->write, "attribute has no write handler");
    return attr->write(NULL, attr, &value, sizeof(value), 0, 0);
}

/* Snapshot of the constructed default, taken once before any case runs (ztest runs
 * cases in name order; the suite-setup snapshot makes the defaults assertion
 * order-independent — same rationale as the shuffle_service suite). */
static uint32_t sDefaultActiveAnimation;
static uint32_t sDefaultDisplayRate;
static uint32_t sDefaultRenderRate;

static void *snapshot_defaults(void) {
    const struct bt_gatt_attr *attr = find_value_attr(4);
    sDefaultActiveAnimation = (attr != NULL) ? read_u32(attr) : 0xdead;
    sDefaultDisplayRate = (find_value_attr(1) != NULL) ? read_u32(find_value_attr(1)) : 0xdead;
    sDefaultRenderRate = (find_value_attr(2) != NULL) ? read_u32(find_value_attr(2)) : 0xdead;
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

/* ---------------------------------------------------------------------------
 * Issue #376: the render rate defaults to the display rate, and
 * getRenderRateMs() floors the render interval at the display interval (a
 * shorter render interval only produces frames the display never samples).
 * The floor clamps the RETURNED value only — there is NO write-back and NO
 * migration at all (PR #378 review, three rounds: every write-back variant
 * re-armed on some later display-rate change and destroyed a value the user
 * may have meant). A stale persisted 11100 stays stored and is floored at
 * point of use like any other below-display value.
 * ------------------------------------------------------------------------- */

ZTEST(core_config_service, test_render_rate_default_matches_display_rate) {
    zassert_equal(sDefaultDisplayRate, 33300, "Display Thread Rate must default to 33300");
    zassert_equal(sDefaultRenderRate, 33300,
                  "Render Thread Rate must default to the display rate (33300)");
    zassert_within(CoreConfig::getInstance().getDisplayRateMs(), 33.3f, 0.001f,
                   "getDisplayRateMs must decode the default as 33.3 ms");
    zassert_within(CoreConfig::getInstance().getRenderRateMs(), 33.3f, 0.001f,
                   "getRenderRateMs must decode the default as 33.3 ms");
}

ZTEST(core_config_service, test_render_rate_below_display_write_rejected) {
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(render);

    /* PR #378 review round 6: a remote render-rate write below the current
     * display rate used to be a silent no-op — accepted, persisted, read back
     * as stored (so the app UI showed it applied) while getRenderRateMs()
     * floored it with no notify and no error. It is now rejected with an ATT
     * error; the app reverts the field from its cached previous value and
     * surfaces a write error (it does not read back on failure — see the
     * characteristic's doc block in core_config.cpp, PR #378 review round 8). */
    ssize_t ret = gatt_write_u32(render, 20000);
    zassert_true(ret < 0, "a below-display render write must be ATT-rejected, got %zd", ret);
    zassert_equal(read_u32(render), sDefaultRenderRate,
                  "a rejected write must roll the stored value back");
    zassert_within(CoreConfig::getInstance().getRenderRateMs(), 33.3f, 0.001f,
                   "a rejected write must leave the effective rate untouched");

    /* Boundary: exactly the display rate is allowed (1:1 is the default). */
    zassert_equal(gatt_write_u32(render, 33300), (ssize_t)sizeof(uint32_t),
                  "a render write EQUAL to the display rate must be accepted");
}

ZTEST(core_config_service, test_render_rate_noop_rewrite_accepted_below_display) {
    const struct bt_gatt_attr *display = find_value_attr(1);
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(display);
    zassert_not_null(render);

    /* PR #378 review round 9: on an upgraded board the stored render rate is the
     * legacy 11100 under a 33300 display — the Core Config screen shows 11100,
     * and the user re-committing that visible value (or an app-side replay
     * pushing back what it just read) must NOT be rejected: it asks the device
     * to keep exactly what it already stores. Stage the stored-below-display
     * state via the legal display-lower/raise dance, then rewrite it. */
    gatt_write_u32(display, 11100);
    gatt_write_u32(render, 11100);
    gatt_write_u32(display, 33300);

    zassert_equal(gatt_write_u32(render, 11100), (ssize_t)sizeof(uint32_t),
                  "rewriting the stored value must be accepted even below the display rate");
    zassert_equal(read_u32(render), 11100, "the no-op rewrite must keep the stored value");
    zassert_within(CoreConfig::getInstance().getRenderRateMs(), 33.3f, 0.001f,
                   "the effective rate must stay floored at the display rate");

    /* A DIFFERENT below-display value must still be rejected — the no-op
     * exception must not weaken the new-divergence guard. */
    zassert_true(gatt_write_u32(render, 12000) < 0,
                 "a CHANGED below-display value must still be rejected");
    zassert_equal(read_u32(render), 11100, "the rejected change must roll back");

    gatt_write_u32(display, 11100);  // restore for order-independence (legal order)
    gatt_write_u32(render, sDefaultRenderRate);
    gatt_write_u32(display, sDefaultDisplayRate);
}

ZTEST(core_config_service, test_display_rate_out_of_range_write_rejected) {
    const struct bt_gatt_attr *display = find_value_attr(1);
    zassert_not_null(display);

    /* PR #378 review round 9: the display rate was remotely writable with NO
     * range validation while every render-side clock is now floored at it — one
     * bad write (0 = spinner; 0xFFFFFFFF = ~71 min/frame) stalled BOTH threads
     * and persisted. Out-of-range writes are rejected at the GATT layer. */
    zassert_true(gatt_write_u32(display, 0) < 0, "display rate 0 must be rejected");
    zassert_true(gatt_write_u32(display, 999) < 0, "a sub-ms display rate must be rejected");
    zassert_true(gatt_write_u32(display, 1000001) < 0,
                 "a display rate above 1 s/frame must be rejected");
    zassert_true(gatt_write_u32(display, 0xFFFFFFFFu) < 0,
                 "a ~71 min/frame display rate must be rejected");
    zassert_equal(read_u32(display), sDefaultDisplayRate,
                  "rejected writes must leave the stored display rate untouched");

    /* Boundaries are inclusive. */
    zassert_equal(gatt_write_u32(display, 1000), (ssize_t)sizeof(uint32_t),
                  "the 1 ms floor boundary must be accepted");
    zassert_equal(gatt_write_u32(display, 1000000), (ssize_t)sizeof(uint32_t),
                  "the 1 s ceiling boundary must be accepted");

    gatt_write_u32(display, sDefaultDisplayRate);  // restore for order-independence
}

ZTEST(core_config_service, test_render_rate_stale_default_floored_not_rewritten) {
    const struct bt_gatt_attr *display = find_value_attr(1);
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(display);
    zassert_not_null(render);

    /* The old pre-#376 default, as persisted on already-deployed boards: the
     * effective rate is floored at the display interval, and the stored value
     * is deliberately NOT rewritten — no migration exists (PR #378 review:
     * every write-back variant re-armed later and destroyed a deliberate
     * 90 Hz setup, which holds exactly this value). A below-display GATT write
     * is rejected now (round 6), so stage the stored-below-display state the
     * way it arises legally at runtime: set 11100/11100, then raise the
     * display back — the settings-load path (doLoad) reaches the same state on
     * real upgraded boards and equally bypasses the write-time gate. */
    gatt_write_u32(display, 11100);
    gatt_write_u32(render, 11100);
    gatt_write_u32(display, 33300);

    float rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 33.3f, 0.001f, "a stale 11100 must be floored at the display rate");
    zassert_equal(read_u32(render), 11100, "the stored value must not be rewritten");
    zassert_equal(read_u32(display), 33300, "the display rate must be untouched");

    gatt_write_u32(render, sDefaultRenderRate);  // restore for order-independence
}

ZTEST(core_config_service, test_render_rate_11100_kept_when_display_matches) {
    const struct bt_gatt_attr *display = find_value_attr(1);
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(display);
    zassert_not_null(render);

    /* A deliberate ~90 Hz setup (display = render = 11100) must be honored in
     * full — the value is stored, effective, and never rewritten (PR #378
     * review: a migration write-back stole this configuration and made 11100
     * permanently unsettable). Since round 6 the order is display-FIRST: the
     * render-first order is ATT-rejected (see
     * test_render_rate_below_display_write_rejected), which is the app's cue
     * to lower the display rate before the render rate. */
    zassert_true(gatt_write_u32(render, 11100) < 0,
                 "render-first must be rejected while the display rate is 33300");
    gatt_write_u32(display, 11100);
    zassert_equal(gatt_write_u32(render, 11100), (ssize_t)sizeof(uint32_t),
                  "the same write must succeed once the display rate matches");

    float rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 11.1f, 0.001f, "a deliberate 90 Hz setup must be honored");
    zassert_equal(read_u32(render), 11100, "the 90 Hz render setting must not be rewritten");

    gatt_write_u32(display, sDefaultDisplayRate);  // restore for order-independence
    gatt_write_u32(render, sDefaultRenderRate);
}

ZTEST(core_config_service, test_render_rate_floor_preserves_user_value) {
    const struct bt_gatt_attr *display = find_value_attr(1);
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(display);
    zassert_not_null(render);

    /* A user's own legally-written value becomes below-display when the
     * display rate is later raised; it is floored at point of use but never
     * rewritten — the setting survives. (Direct below-display writes are
     * rejected since round 6, so the raise-the-display-later path is the one
     * way this state still arises over BLE. 30000 is an exact 2x multiple of
     * the 15000 display, so the snap stores it as written.) */
    gatt_write_u32(display, 15000);
    gatt_write_u32(render, 30000);
    gatt_write_u32(display, sDefaultDisplayRate);  // raise back above the render rate

    float rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 33.3f, 0.001f, "render interval must be floored at the display's");
    zassert_equal(read_u32(render), 30000, "the floor must not destroy the user's setting");

    /* ...and becomes effective again once the display rate drops below it. */
    gatt_write_u32(display, 15000);
    rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 30.0f, 0.001f, "the preserved value must apply when legal again");

    gatt_write_u32(display, sDefaultDisplayRate);  // restore
    gatt_write_u32(render, sDefaultRenderRate);
}

ZTEST(core_config_service, test_render_rate_slower_than_display_allowed) {
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(render);

    /* Rendering slower than the display is allowed — and since the divider
     * renders once per ceil(render/display) frames, the write is snapped UP to
     * the exact multiple it will actually run at (50000 under a 33300 display
     * really renders every 66600), so read-back reports the true rate
     * (PR #381 review round 9). */
    zassert_equal(gatt_write_u32(render, 50000), (ssize_t)sizeof(uint32_t),
                  "a slower-than-display write must be accepted");
    zassert_equal(read_u32(render), 66600,
                  "a non-multiple must be snapped to the next display multiple");
    float rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 66.6f, 0.001f, "the effective rate must equal the stored snap");

    gatt_write_u32(render, sDefaultRenderRate);  // restore for order-independence
}

ZTEST(core_config_service, test_render_rate_snap_to_display_multiple) {
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(render);

    /* PR #381 review round 9: a render rate that is not an exact multiple of
     * the display rate would be silently effective at N x display while
     * reading back as written — the same stored-vs-effective divergence class
     * the below-display rejection closes, for a much larger set of values.
     * The write is snapped up to the divider's ceiling multiple instead. */
    gatt_write_u32(render, 100000);  // ratio 3.003 -> N = 4 (strict ceiling)
    zassert_equal(read_u32(render), 133200,
                  "just-above-a-multiple must snap up a full step (ceil semantics)");

    gatt_write_u32(render, 66600);  // exact multiple: stored untouched
    zassert_equal(read_u32(render), 66600, "an exact multiple must be stored as written");

    gatt_write_u32(render, sDefaultRenderRate);  // restore (33300 = exact 1x multiple)
    zassert_equal(read_u32(render), sDefaultRenderRate,
                  "the default must remain storable as itself");
}

ZTEST(core_config_service, test_render_rate_follows_raised_display_rate_reversibly) {
    const struct bt_gatt_attr *display = find_value_attr(1);
    const struct bt_gatt_attr *render = find_value_attr(2);
    zassert_not_null(display);
    zassert_not_null(render);

    /* Slowing the display below the render rate must floor the effective render
     * rate — the render knob itself never moved (writing it below the raised
     * display would be rejected now, see the round-6 rejection test; the stored
     * 33300 predates the display change, which is exactly the divergence this
     * pins as reversible)... */
    gatt_write_u32(render, 33300);
    gatt_write_u32(display, 50000);

    float rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 50.0f, 0.001f, "raising the display interval must floor the render");
    zassert_equal(read_u32(render), 33300,
                  "the floor must NOT rewrite the characteristic (PR #378 review)");

    /* ...and restoring the display rate must restore the render rate — the
     * floor is reversible because it never destroyed the stored value. */
    gatt_write_u32(display, sDefaultDisplayRate);
    rateMs = CoreConfig::getInstance().getRenderRateMs();
    zassert_within(rateMs, 33.3f, 0.001f, "the render rate must recover with the display rate");
    zassert_equal(read_u32(render), 33300, "the stored render rate must be untouched");

    gatt_write_u32(render, sDefaultRenderRate);  // restore for order-independence
}
